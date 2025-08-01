// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES
 */
#include <linux/iommu.h>
#include <linux/iommufd.h>
#include <linux/pci-ats.h>
#include <linux/slab.h>
#include <uapi/linux/iommufd.h>

#include "../iommu-priv.h"
#include "io_pagetable.h"
#include "iommufd_private.h"

static bool allow_unsafe_interrupts;
module_param(allow_unsafe_interrupts, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(
	allow_unsafe_interrupts,
	"Allow IOMMUFD to bind to devices even if the platform cannot isolate "
	"the MSI interrupt window. Enabling this is a security weakness.");

struct iommufd_attach {
	struct iommufd_hw_pagetable *hwpt;
	struct xarray device_array;
};

static void iommufd_group_release(struct kref *kref)
{
	struct iommufd_group *igroup =
		container_of(kref, struct iommufd_group, ref);

	WARN_ON(!xa_empty(&igroup->pasid_attach));

	xa_cmpxchg(&igroup->ictx->groups, iommu_group_id(igroup->group), igroup,
		   NULL, GFP_KERNEL);
	iommu_group_put(igroup->group);
	mutex_destroy(&igroup->lock);
	kfree(igroup);
}

static void iommufd_put_group(struct iommufd_group *group)
{
	kref_put(&group->ref, iommufd_group_release);
}

static bool iommufd_group_try_get(struct iommufd_group *igroup,
				  struct iommu_group *group)
{
	if (!igroup)
		return false;
	/*
	 * group ID's cannot be re-used until the group is put back which does
	 * not happen if we could get an igroup pointer under the xa_lock.
	 */
	if (WARN_ON(igroup->group != group))
		return false;
	return kref_get_unless_zero(&igroup->ref);
}

/*
 * iommufd needs to store some more data for each iommu_group, we keep a
 * parallel xarray indexed by iommu_group id to hold this instead of putting it
 * in the core structure. To keep things simple the iommufd_group memory is
 * unique within the iommufd_ctx. This makes it easy to check there are no
 * memory leaks.
 */
static struct iommufd_group *iommufd_get_group(struct iommufd_ctx *ictx,
					       struct device *dev)
{
	struct iommufd_group *new_igroup;
	struct iommufd_group *cur_igroup;
	struct iommufd_group *igroup;
	struct iommu_group *group;
	unsigned int id;

	group = iommu_group_get(dev);
	if (!group)
		return ERR_PTR(-ENODEV);

	id = iommu_group_id(group);

	xa_lock(&ictx->groups);
	igroup = xa_load(&ictx->groups, id);
	if (iommufd_group_try_get(igroup, group)) {
		xa_unlock(&ictx->groups);
		iommu_group_put(group);
		return igroup;
	}
	xa_unlock(&ictx->groups);

	new_igroup = kzalloc(sizeof(*new_igroup), GFP_KERNEL);
	if (!new_igroup) {
		iommu_group_put(group);
		return ERR_PTR(-ENOMEM);
	}

	kref_init(&new_igroup->ref);
	mutex_init(&new_igroup->lock);
	xa_init(&new_igroup->pasid_attach);
	new_igroup->sw_msi_start = PHYS_ADDR_MAX;
	/* group reference moves into new_igroup */
	new_igroup->group = group;

	/*
	 * The ictx is not additionally refcounted here becase all objects using
	 * an igroup must put it before their destroy completes.
	 */
	new_igroup->ictx = ictx;

	/*
	 * We dropped the lock so igroup is invalid. NULL is a safe and likely
	 * value to assume for the xa_cmpxchg algorithm.
	 */
	cur_igroup = NULL;
	xa_lock(&ictx->groups);
	while (true) {
		igroup = __xa_cmpxchg(&ictx->groups, id, cur_igroup, new_igroup,
				      GFP_KERNEL);
		if (xa_is_err(igroup)) {
			xa_unlock(&ictx->groups);
			iommufd_put_group(new_igroup);
			return ERR_PTR(xa_err(igroup));
		}

		/* new_group was successfully installed */
		if (cur_igroup == igroup) {
			xa_unlock(&ictx->groups);
			return new_igroup;
		}

		/* Check again if the current group is any good */
		if (iommufd_group_try_get(igroup, group)) {
			xa_unlock(&ictx->groups);
			iommufd_put_group(new_igroup);
			return igroup;
		}
		cur_igroup = igroup;
	}
}

static void iommufd_device_remove_vdev(struct iommufd_device *idev)
{
	struct iommufd_vdevice *vdev;

	mutex_lock(&idev->igroup->lock);
	/* prevent new references from vdev */
	idev->destroying = true;
	/* vdev has been completely destroyed by userspace */
	if (!idev->vdev)
		goto out_unlock;

	vdev = iommufd_get_vdevice(idev->ictx, idev->vdev->obj.id);
	/*
	 * An ongoing vdev destroy ioctl has removed the vdev from the object
	 * xarray, but has not finished iommufd_vdevice_destroy() yet as it
	 * needs the same mutex. We exit the locking then wait on wait_cnt
	 * reference for the vdev destruction.
	 */
	if (IS_ERR(vdev))
		goto out_unlock;

	/* Should never happen */
	if (WARN_ON(vdev != idev->vdev)) {
		iommufd_put_object(idev->ictx, &vdev->obj);
		goto out_unlock;
	}

	/*
	 * vdev is still alive. Hold a users refcount to prevent racing with
	 * userspace destruction, then use iommufd_object_tombstone_user() to
	 * destroy it and leave a tombstone.
	 */
	refcount_inc(&vdev->obj.users);
	iommufd_put_object(idev->ictx, &vdev->obj);
	mutex_unlock(&idev->igroup->lock);
	iommufd_object_tombstone_user(idev->ictx, &vdev->obj);
	return;

out_unlock:
	mutex_unlock(&idev->igroup->lock);
}

void iommufd_device_pre_destroy(struct iommufd_object *obj)
{
	struct iommufd_device *idev =
		container_of(obj, struct iommufd_device, obj);

	/* Release the wait_cnt reference on this */
	iommufd_device_remove_vdev(idev);
}

void iommufd_device_destroy(struct iommufd_object *obj)
{
	struct iommufd_device *idev =
		container_of(obj, struct iommufd_device, obj);

	iommu_device_release_dma_owner(idev->dev);
	iommufd_put_group(idev->igroup);
	if (!iommufd_selftest_is_mock_dev(idev->dev))
		iommufd_ctx_put(idev->ictx);
}

/**
 * iommufd_device_bind - Bind a physical device to an iommu fd
 * @ictx: iommufd file descriptor
 * @dev: Pointer to a physical device struct
 * @id: Output ID number to return to userspace for this device
 *
 * A successful bind establishes an ownership over the device and returns
 * struct iommufd_device pointer, otherwise returns error pointer.
 *
 * A driver using this API must set driver_managed_dma and must not touch
 * the device until this routine succeeds and establishes ownership.
 *
 * Binding a PCI device places the entire RID under iommufd control.
 *
 * The caller must undo this with iommufd_device_unbind()
 */
struct iommufd_device *iommufd_device_bind(struct iommufd_ctx *ictx,
					   struct device *dev, u32 *id)
{
	struct iommufd_device *idev;
	struct iommufd_group *igroup;
	int rc;

	/*
	 * iommufd always sets IOMMU_CACHE because we offer no way for userspace
	 * to restore cache coherency.
	 */
	if (!device_iommu_capable(dev, IOMMU_CAP_CACHE_COHERENCY))
		return ERR_PTR(-EINVAL);

	igroup = iommufd_get_group(ictx, dev);
	if (IS_ERR(igroup))
		return ERR_CAST(igroup);

	/*
	 * For historical compat with VFIO the insecure interrupt path is
	 * allowed if the module parameter is set. Secure/Isolated means that a
	 * MemWr operation from the device (eg a simple DMA) cannot trigger an
	 * interrupt outside this iommufd context.
	 */
	if (!iommufd_selftest_is_mock_dev(dev) &&
	    !iommu_group_has_isolated_msi(igroup->group)) {
		if (!allow_unsafe_interrupts) {
			rc = -EPERM;
			goto out_group_put;
		}

		dev_warn(
			dev,
			"MSI interrupts are not secure, they cannot be isolated by the platform. "
			"Check that platform features like interrupt remapping are enabled. "
			"Use the \"allow_unsafe_interrupts\" module parameter to override\n");
	}

	rc = iommu_device_claim_dma_owner(dev, ictx);
	if (rc)
		goto out_group_put;

	idev = iommufd_object_alloc(ictx, idev, IOMMUFD_OBJ_DEVICE);
	if (IS_ERR(idev)) {
		rc = PTR_ERR(idev);
		goto out_release_owner;
	}
	idev->ictx = ictx;
	if (!iommufd_selftest_is_mock_dev(dev))
		iommufd_ctx_get(ictx);
	idev->dev = dev;
	idev->enforce_cache_coherency =
		device_iommu_capable(dev, IOMMU_CAP_ENFORCE_CACHE_COHERENCY);
	/* The calling driver is a user until iommufd_device_unbind() */
	refcount_inc(&idev->obj.users);
	/* igroup refcount moves into iommufd_device */
	idev->igroup = igroup;

	/*
	 * If the caller fails after this success it must call
	 * iommufd_unbind_device() which is safe since we hold this refcount.
	 * This also means the device is a leaf in the graph and no other object
	 * can take a reference on it.
	 */
	iommufd_object_finalize(ictx, &idev->obj);
	*id = idev->obj.id;
	return idev;

out_release_owner:
	iommu_device_release_dma_owner(dev);
out_group_put:
	iommufd_put_group(igroup);
	return ERR_PTR(rc);
}
EXPORT_SYMBOL_NS_GPL(iommufd_device_bind, "IOMMUFD");

/**
 * iommufd_ctx_has_group - True if any device within the group is bound
 *                         to the ictx
 * @ictx: iommufd file descriptor
 * @group: Pointer to a physical iommu_group struct
 *
 * True if any device within the group has been bound to this ictx, ex. via
 * iommufd_device_bind(), therefore implying ictx ownership of the group.
 */
bool iommufd_ctx_has_group(struct iommufd_ctx *ictx, struct iommu_group *group)
{
	struct iommufd_object *obj;
	unsigned long index;

	if (!ictx || !group)
		return false;

	xa_lock(&ictx->objects);
	xa_for_each(&ictx->objects, index, obj) {
		if (obj->type == IOMMUFD_OBJ_DEVICE &&
		    container_of(obj, struct iommufd_device, obj)
				    ->igroup->group == group) {
			xa_unlock(&ictx->objects);
			return true;
		}
	}
	xa_unlock(&ictx->objects);
	return false;
}
EXPORT_SYMBOL_NS_GPL(iommufd_ctx_has_group, "IOMMUFD");

/**
 * iommufd_device_unbind - Undo iommufd_device_bind()
 * @idev: Device returned by iommufd_device_bind()
 *
 * Release the device from iommufd control. The DMA ownership will return back
 * to unowned with DMA controlled by the DMA API. This invalidates the
 * iommufd_device pointer, other APIs that consume it must not be called
 * concurrently.
 */
void iommufd_device_unbind(struct iommufd_device *idev)
{
	iommufd_object_destroy_user(idev->ictx, &idev->obj);
}
EXPORT_SYMBOL_NS_GPL(iommufd_device_unbind, "IOMMUFD");

struct iommufd_ctx *iommufd_device_to_ictx(struct iommufd_device *idev)
{
	return idev->ictx;
}
EXPORT_SYMBOL_NS_GPL(iommufd_device_to_ictx, "IOMMUFD");

u32 iommufd_device_to_id(struct iommufd_device *idev)
{
	return idev->obj.id;
}
EXPORT_SYMBOL_NS_GPL(iommufd_device_to_id, "IOMMUFD");

static unsigned int iommufd_group_device_num(struct iommufd_group *igroup,
					     ioasid_t pasid)
{
	struct iommufd_attach *attach;
	struct iommufd_device *idev;
	unsigned int count = 0;
	unsigned long index;

	lockdep_assert_held(&igroup->lock);

	attach = xa_load(&igroup->pasid_attach, pasid);
	if (attach)
		xa_for_each(&attach->device_array, index, idev)
			count++;
	return count;
}

#ifdef CONFIG_IRQ_MSI_IOMMU
static int iommufd_group_setup_msi(struct iommufd_group *igroup,
				   struct iommufd_hwpt_paging *hwpt_paging)
{
	struct iommufd_ctx *ictx = igroup->ictx;
	struct iommufd_sw_msi_map *cur;

	if (igroup->sw_msi_start == PHYS_ADDR_MAX)
		return 0;

	/*
	 * Install all the MSI pages the device has been using into the domain
	 */
	guard(mutex)(&ictx->sw_msi_lock);
	list_for_each_entry(cur, &ictx->sw_msi_list, sw_msi_item) {
		int rc;

		if (cur->sw_msi_start != igroup->sw_msi_start ||
		    !test_bit(cur->id, igroup->required_sw_msi.bitmap))
			continue;

		rc = iommufd_sw_msi_install(ictx, hwpt_paging, cur);
		if (rc)
			return rc;
	}
	return 0;
}
#else
static inline int
iommufd_group_setup_msi(struct iommufd_group *igroup,
			struct iommufd_hwpt_paging *hwpt_paging)
{
	return 0;
}
#endif

static bool
iommufd_group_first_attach(struct iommufd_group *igroup, ioasid_t pasid)
{
	lockdep_assert_held(&igroup->lock);
	return !xa_load(&igroup->pasid_attach, pasid);
}

static int
iommufd_device_attach_reserved_iova(struct iommufd_device *idev,
				    struct iommufd_hwpt_paging *hwpt_paging)
{
	struct iommufd_group *igroup = idev->igroup;
	int rc;

	lockdep_assert_held(&igroup->lock);

	rc = iopt_table_enforce_dev_resv_regions(&hwpt_paging->ioas->iopt,
						 idev->dev,
						 &igroup->sw_msi_start);
	if (rc)
		return rc;

	if (iommufd_group_first_attach(igroup, IOMMU_NO_PASID)) {
		rc = iommufd_group_setup_msi(igroup, hwpt_paging);
		if (rc) {
			iopt_remove_reserved_iova(&hwpt_paging->ioas->iopt,
						  idev->dev);
			return rc;
		}
	}
	return 0;
}

/* The device attach/detach/replace helpers for attach_handle */

static bool iommufd_device_is_attached(struct iommufd_device *idev,
				       ioasid_t pasid)
{
	struct iommufd_attach *attach;

	attach = xa_load(&idev->igroup->pasid_attach, pasid);
	return xa_load(&attach->device_array, idev->obj.id);
}

static int iommufd_hwpt_pasid_compat(struct iommufd_hw_pagetable *hwpt,
				     struct iommufd_device *idev,
				     ioasid_t pasid)
{
	struct iommufd_group *igroup = idev->igroup;

	lockdep_assert_held(&igroup->lock);

	if (pasid == IOMMU_NO_PASID) {
		unsigned long start = IOMMU_NO_PASID;

		if (!hwpt->pasid_compat &&
		    xa_find_after(&igroup->pasid_attach,
				  &start, UINT_MAX, XA_PRESENT))
			return -EINVAL;
	} else {
		struct iommufd_attach *attach;

		if (!hwpt->pasid_compat)
			return -EINVAL;

		attach = xa_load(&igroup->pasid_attach, IOMMU_NO_PASID);
		if (attach && attach->hwpt && !attach->hwpt->pasid_compat)
			return -EINVAL;
	}

	return 0;
}

static bool iommufd_hwpt_compatible_device(struct iommufd_hw_pagetable *hwpt,
					   struct iommufd_device *idev)
{
	struct pci_dev *pdev;

	if (!hwpt->fault || !dev_is_pci(idev->dev))
		return true;

	/*
	 * Once we turn on PCI/PRI support for VF, the response failure code
	 * should not be forwarded to the hardware due to PRI being a shared
	 * resource between PF and VFs. There is no coordination for this
	 * shared capability. This waits for a vPRI reset to recover.
	 */
	pdev = to_pci_dev(idev->dev);

	return (!pdev->is_virtfn || !pci_pri_supported(pdev));
}

static int iommufd_hwpt_attach_device(struct iommufd_hw_pagetable *hwpt,
				      struct iommufd_device *idev,
				      ioasid_t pasid)
{
	struct iommufd_attach_handle *handle;
	int rc;

	if (!iommufd_hwpt_compatible_device(hwpt, idev))
		return -EINVAL;

	rc = iommufd_hwpt_pasid_compat(hwpt, idev, pasid);
	if (rc)
		return rc;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->idev = idev;
	if (pasid == IOMMU_NO_PASID)
		rc = iommu_attach_group_handle(hwpt->domain, idev->igroup->group,
					       &handle->handle);
	else
		rc = iommu_attach_device_pasid(hwpt->domain, idev->dev, pasid,
					       &handle->handle);
	if (rc)
		goto out_free_handle;

	return 0;

out_free_handle:
	kfree(handle);
	return rc;
}

static struct iommufd_attach_handle *
iommufd_device_get_attach_handle(struct iommufd_device *idev, ioasid_t pasid)
{
	struct iommu_attach_handle *handle;

	lockdep_assert_held(&idev->igroup->lock);

	handle = iommu_attach_handle_get(idev->igroup->group, pasid, 0);
	if (IS_ERR(handle))
		return NULL;
	return to_iommufd_handle(handle);
}

static void iommufd_hwpt_detach_device(struct iommufd_hw_pagetable *hwpt,
				       struct iommufd_device *idev,
				       ioasid_t pasid)
{
	struct iommufd_attach_handle *handle;

	handle = iommufd_device_get_attach_handle(idev, pasid);
	if (pasid == IOMMU_NO_PASID)
		iommu_detach_group_handle(hwpt->domain, idev->igroup->group);
	else
		iommu_detach_device_pasid(hwpt->domain, idev->dev, pasid);

	iommufd_auto_response_faults(hwpt, handle);
	kfree(handle);
}

static int iommufd_hwpt_replace_device(struct iommufd_device *idev,
				       ioasid_t pasid,
				       struct iommufd_hw_pagetable *hwpt,
				       struct iommufd_hw_pagetable *old)
{
	struct iommufd_attach_handle *handle, *old_handle;
	int rc;

	if (!iommufd_hwpt_compatible_device(hwpt, idev))
		return -EINVAL;

	rc = iommufd_hwpt_pasid_compat(hwpt, idev, pasid);
	if (rc)
		return rc;

	old_handle = iommufd_device_get_attach_handle(idev, pasid);

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->idev = idev;
	if (pasid == IOMMU_NO_PASID)
		rc = iommu_replace_group_handle(idev->igroup->group,
						hwpt->domain, &handle->handle);
	else
		rc = iommu_replace_device_pasid(hwpt->domain, idev->dev,
						pasid, &handle->handle);
	if (rc)
		goto out_free_handle;

	iommufd_auto_response_faults(hwpt, old_handle);
	kfree(old_handle);

	return 0;

out_free_handle:
	kfree(handle);
	return rc;
}

int iommufd_hw_pagetable_attach(struct iommufd_hw_pagetable *hwpt,
				struct iommufd_device *idev, ioasid_t pasid)
{
	struct iommufd_hwpt_paging *hwpt_paging = find_hwpt_paging(hwpt);
	bool attach_resv = hwpt_paging && pasid == IOMMU_NO_PASID;
	struct iommufd_group *igroup = idev->igroup;
	struct iommufd_hw_pagetable *old_hwpt;
	struct iommufd_attach *attach;
	int rc;

	mutex_lock(&igroup->lock);

	attach = xa_cmpxchg(&igroup->pasid_attach, pasid, NULL,
			    XA_ZERO_ENTRY, GFP_KERNEL);
	if (xa_is_err(attach)) {
		rc = xa_err(attach);
		goto err_unlock;
	}

	if (!attach) {
		attach = kzalloc(sizeof(*attach), GFP_KERNEL);
		if (!attach) {
			rc = -ENOMEM;
			goto err_release_pasid;
		}
		xa_init(&attach->device_array);
	}

	old_hwpt = attach->hwpt;

	rc = xa_insert(&attach->device_array, idev->obj.id, XA_ZERO_ENTRY,
		       GFP_KERNEL);
	if (rc) {
		WARN_ON(rc == -EBUSY && !old_hwpt);
		goto err_free_attach;
	}

	if (old_hwpt && old_hwpt != hwpt) {
		rc = -EINVAL;
		goto err_release_devid;
	}

	if (attach_resv) {
		rc = iommufd_device_attach_reserved_iova(idev, hwpt_paging);
		if (rc)
			goto err_release_devid;
	}

	/*
	 * Only attach to the group once for the first device that is in the
	 * group. All the other devices will follow this attachment. The user
	 * should attach every device individually to the hwpt as the per-device
	 * reserved regions are only updated during individual device
	 * attachment.
	 */
	if (iommufd_group_first_attach(igroup, pasid)) {
		rc = iommufd_hwpt_attach_device(hwpt, idev, pasid);
		if (rc)
			goto err_unresv;
		attach->hwpt = hwpt;
		WARN_ON(xa_is_err(xa_store(&igroup->pasid_attach, pasid, attach,
					   GFP_KERNEL)));
	}
	refcount_inc(&hwpt->obj.users);
	WARN_ON(xa_is_err(xa_store(&attach->device_array, idev->obj.id,
				   idev, GFP_KERNEL)));
	mutex_unlock(&igroup->lock);
	return 0;
err_unresv:
	if (attach_resv)
		iopt_remove_reserved_iova(&hwpt_paging->ioas->iopt, idev->dev);
err_release_devid:
	xa_release(&attach->device_array, idev->obj.id);
err_free_attach:
	if (iommufd_group_first_attach(igroup, pasid))
		kfree(attach);
err_release_pasid:
	if (iommufd_group_first_attach(igroup, pasid))
		xa_release(&igroup->pasid_attach, pasid);
err_unlock:
	mutex_unlock(&igroup->lock);
	return rc;
}

struct iommufd_hw_pagetable *
iommufd_hw_pagetable_detach(struct iommufd_device *idev, ioasid_t pasid)
{
	struct iommufd_group *igroup = idev->igroup;
	struct iommufd_hwpt_paging *hwpt_paging;
	struct iommufd_hw_pagetable *hwpt;
	struct iommufd_attach *attach;

	mutex_lock(&igroup->lock);
	attach = xa_load(&igroup->pasid_attach, pasid);
	if (!attach) {
		mutex_unlock(&igroup->lock);
		return NULL;
	}

	hwpt = attach->hwpt;
	hwpt_paging = find_hwpt_paging(hwpt);

	xa_erase(&attach->device_array, idev->obj.id);
	if (xa_empty(&attach->device_array)) {
		iommufd_hwpt_detach_device(hwpt, idev, pasid);
		xa_erase(&igroup->pasid_attach, pasid);
		kfree(attach);
	}
	if (hwpt_paging && pasid == IOMMU_NO_PASID)
		iopt_remove_reserved_iova(&hwpt_paging->ioas->iopt, idev->dev);
	mutex_unlock(&igroup->lock);

	/* Caller must destroy hwpt */
	return hwpt;
}

static struct iommufd_hw_pagetable *
iommufd_device_do_attach(struct iommufd_device *idev, ioasid_t pasid,
			 struct iommufd_hw_pagetable *hwpt)
{
	int rc;

	rc = iommufd_hw_pagetable_attach(hwpt, idev, pasid);
	if (rc)
		return ERR_PTR(rc);
	return NULL;
}

static void
iommufd_group_remove_reserved_iova(struct iommufd_group *igroup,
				   struct iommufd_hwpt_paging *hwpt_paging)
{
	struct iommufd_attach *attach;
	struct iommufd_device *cur;
	unsigned long index;

	lockdep_assert_held(&igroup->lock);

	attach = xa_load(&igroup->pasid_attach, IOMMU_NO_PASID);
	xa_for_each(&attach->device_array, index, cur)
		iopt_remove_reserved_iova(&hwpt_paging->ioas->iopt, cur->dev);
}

static int
iommufd_group_do_replace_reserved_iova(struct iommufd_group *igroup,
				       struct iommufd_hwpt_paging *hwpt_paging)
{
	struct iommufd_hwpt_paging *old_hwpt_paging;
	struct iommufd_attach *attach;
	struct iommufd_device *cur;
	unsigned long index;
	int rc;

	lockdep_assert_held(&igroup->lock);

	attach = xa_load(&igroup->pasid_attach, IOMMU_NO_PASID);
	old_hwpt_paging = find_hwpt_paging(attach->hwpt);
	if (!old_hwpt_paging || hwpt_paging->ioas != old_hwpt_paging->ioas) {
		xa_for_each(&attach->device_array, index, cur) {
			rc = iopt_table_enforce_dev_resv_regions(
				&hwpt_paging->ioas->iopt, cur->dev, NULL);
			if (rc)
				goto err_unresv;
		}
	}

	rc = iommufd_group_setup_msi(igroup, hwpt_paging);
	if (rc)
		goto err_unresv;
	return 0;

err_unresv:
	iommufd_group_remove_reserved_iova(igroup, hwpt_paging);
	return rc;
}

static struct iommufd_hw_pagetable *
iommufd_device_do_replace(struct iommufd_device *idev, ioasid_t pasid,
			  struct iommufd_hw_pagetable *hwpt)
{
	struct iommufd_hwpt_paging *hwpt_paging = find_hwpt_paging(hwpt);
	bool attach_resv = hwpt_paging && pasid == IOMMU_NO_PASID;
	struct iommufd_hwpt_paging *old_hwpt_paging;
	struct iommufd_group *igroup = idev->igroup;
	struct iommufd_hw_pagetable *old_hwpt;
	struct iommufd_attach *attach;
	unsigned int num_devices;
	int rc;

	mutex_lock(&igroup->lock);

	attach = xa_load(&igroup->pasid_attach, pasid);
	if (!attach) {
		rc = -EINVAL;
		goto err_unlock;
	}

	old_hwpt = attach->hwpt;

	WARN_ON(!old_hwpt || xa_empty(&attach->device_array));

	if (!iommufd_device_is_attached(idev, pasid)) {
		rc = -EINVAL;
		goto err_unlock;
	}

	if (hwpt == old_hwpt) {
		mutex_unlock(&igroup->lock);
		return NULL;
	}

	if (attach_resv) {
		rc = iommufd_group_do_replace_reserved_iova(igroup, hwpt_paging);
		if (rc)
			goto err_unlock;
	}

	rc = iommufd_hwpt_replace_device(idev, pasid, hwpt, old_hwpt);
	if (rc)
		goto err_unresv;

	old_hwpt_paging = find_hwpt_paging(old_hwpt);
	if (old_hwpt_paging && pasid == IOMMU_NO_PASID &&
	    (!hwpt_paging || hwpt_paging->ioas != old_hwpt_paging->ioas))
		iommufd_group_remove_reserved_iova(igroup, old_hwpt_paging);

	attach->hwpt = hwpt;

	num_devices = iommufd_group_device_num(igroup, pasid);
	/*
	 * Move the refcounts held by the device_array to the new hwpt. Retain a
	 * refcount for this thread as the caller will free it.
	 */
	refcount_add(num_devices, &hwpt->obj.users);
	if (num_devices > 1)
		WARN_ON(refcount_sub_and_test(num_devices - 1,
					      &old_hwpt->obj.users));
	mutex_unlock(&igroup->lock);

	/* Caller must destroy old_hwpt */
	return old_hwpt;
err_unresv:
	if (attach_resv)
		iommufd_group_remove_reserved_iova(igroup, hwpt_paging);
err_unlock:
	mutex_unlock(&igroup->lock);
	return ERR_PTR(rc);
}

typedef struct iommufd_hw_pagetable *(*attach_fn)(
	struct iommufd_device *idev, ioasid_t pasid,
	struct iommufd_hw_pagetable *hwpt);

/*
 * When automatically managing the domains we search for a compatible domain in
 * the iopt and if one is found use it, otherwise create a new domain.
 * Automatic domain selection will never pick a manually created domain.
 */
static struct iommufd_hw_pagetable *
iommufd_device_auto_get_domain(struct iommufd_device *idev, ioasid_t pasid,
			       struct iommufd_ioas *ioas, u32 *pt_id,
			       attach_fn do_attach)
{
	/*
	 * iommufd_hw_pagetable_attach() is called by
	 * iommufd_hw_pagetable_alloc() in immediate attachment mode, same as
	 * iommufd_device_do_attach(). So if we are in this mode then we prefer
	 * to use the immediate_attach path as it supports drivers that can't
	 * directly allocate a domain.
	 */
	bool immediate_attach = do_attach == iommufd_device_do_attach;
	struct iommufd_hw_pagetable *destroy_hwpt;
	struct iommufd_hwpt_paging *hwpt_paging;
	struct iommufd_hw_pagetable *hwpt;

	/*
	 * There is no differentiation when domains are allocated, so any domain
	 * that is willing to attach to the device is interchangeable with any
	 * other.
	 */
	mutex_lock(&ioas->mutex);
	list_for_each_entry(hwpt_paging, &ioas->hwpt_list, hwpt_item) {
		if (!hwpt_paging->auto_domain)
			continue;

		hwpt = &hwpt_paging->common;
		if (!iommufd_lock_obj(&hwpt->obj))
			continue;
		destroy_hwpt = (*do_attach)(idev, pasid, hwpt);
		if (IS_ERR(destroy_hwpt)) {
			iommufd_put_object(idev->ictx, &hwpt->obj);
			/*
			 * -EINVAL means the domain is incompatible with the
			 * device. Other error codes should propagate to
			 * userspace as failure. Success means the domain is
			 * attached.
			 */
			if (PTR_ERR(destroy_hwpt) == -EINVAL)
				continue;
			goto out_unlock;
		}
		*pt_id = hwpt->obj.id;
		iommufd_put_object(idev->ictx, &hwpt->obj);
		goto out_unlock;
	}

	hwpt_paging = iommufd_hwpt_paging_alloc(idev->ictx, ioas, idev, pasid,
						0, immediate_attach, NULL);
	if (IS_ERR(hwpt_paging)) {
		destroy_hwpt = ERR_CAST(hwpt_paging);
		goto out_unlock;
	}
	hwpt = &hwpt_paging->common;

	if (!immediate_attach) {
		destroy_hwpt = (*do_attach)(idev, pasid, hwpt);
		if (IS_ERR(destroy_hwpt))
			goto out_abort;
	} else {
		destroy_hwpt = NULL;
	}

	hwpt_paging->auto_domain = true;
	*pt_id = hwpt->obj.id;

	iommufd_object_finalize(idev->ictx, &hwpt->obj);
	mutex_unlock(&ioas->mutex);
	return destroy_hwpt;

out_abort:
	iommufd_object_abort_and_destroy(idev->ictx, &hwpt->obj);
out_unlock:
	mutex_unlock(&ioas->mutex);
	return destroy_hwpt;
}

static int iommufd_device_change_pt(struct iommufd_device *idev,
				    ioasid_t pasid,
				    u32 *pt_id, attach_fn do_attach)
{
	struct iommufd_hw_pagetable *destroy_hwpt;
	struct iommufd_object *pt_obj;

	pt_obj = iommufd_get_object(idev->ictx, *pt_id, IOMMUFD_OBJ_ANY);
	if (IS_ERR(pt_obj))
		return PTR_ERR(pt_obj);

	switch (pt_obj->type) {
	case IOMMUFD_OBJ_HWPT_NESTED:
	case IOMMUFD_OBJ_HWPT_PAGING: {
		struct iommufd_hw_pagetable *hwpt =
			container_of(pt_obj, struct iommufd_hw_pagetable, obj);

		destroy_hwpt = (*do_attach)(idev, pasid, hwpt);
		if (IS_ERR(destroy_hwpt))
			goto out_put_pt_obj;
		break;
	}
	case IOMMUFD_OBJ_IOAS: {
		struct iommufd_ioas *ioas =
			container_of(pt_obj, struct iommufd_ioas, obj);

		destroy_hwpt = iommufd_device_auto_get_domain(idev, pasid, ioas,
							      pt_id, do_attach);
		if (IS_ERR(destroy_hwpt))
			goto out_put_pt_obj;
		break;
	}
	default:
		destroy_hwpt = ERR_PTR(-EINVAL);
		goto out_put_pt_obj;
	}
	iommufd_put_object(idev->ictx, pt_obj);

	/* This destruction has to be after we unlock everything */
	if (destroy_hwpt)
		iommufd_hw_pagetable_put(idev->ictx, destroy_hwpt);
	return 0;

out_put_pt_obj:
	iommufd_put_object(idev->ictx, pt_obj);
	return PTR_ERR(destroy_hwpt);
}

/**
 * iommufd_device_attach - Connect a device/pasid to an iommu_domain
 * @idev: device to attach
 * @pasid: pasid to attach
 * @pt_id: Input a IOMMUFD_OBJ_IOAS, or IOMMUFD_OBJ_HWPT_PAGING
 *         Output the IOMMUFD_OBJ_HWPT_PAGING ID
 *
 * This connects the device/pasid to an iommu_domain, either automatically
 * or manually selected. Once this completes the device could do DMA with
 * @pasid. @pasid is IOMMU_NO_PASID if this attach is for no pasid usage.
 *
 * The caller should return the resulting pt_id back to userspace.
 * This function is undone by calling iommufd_device_detach().
 */
int iommufd_device_attach(struct iommufd_device *idev, ioasid_t pasid,
			  u32 *pt_id)
{
	int rc;

	rc = iommufd_device_change_pt(idev, pasid, pt_id,
				      &iommufd_device_do_attach);
	if (rc)
		return rc;

	/*
	 * Pairs with iommufd_device_detach() - catches caller bugs attempting
	 * to destroy a device with an attachment.
	 */
	refcount_inc(&idev->obj.users);
	return 0;
}
EXPORT_SYMBOL_NS_GPL(iommufd_device_attach, "IOMMUFD");

/**
 * iommufd_device_replace - Change the device/pasid's iommu_domain
 * @idev: device to change
 * @pasid: pasid to change
 * @pt_id: Input a IOMMUFD_OBJ_IOAS, or IOMMUFD_OBJ_HWPT_PAGING
 *         Output the IOMMUFD_OBJ_HWPT_PAGING ID
 *
 * This is the same as::
 *
 *   iommufd_device_detach();
 *   iommufd_device_attach();
 *
 * If it fails then no change is made to the attachment. The iommu driver may
 * implement this so there is no disruption in translation. This can only be
 * called if iommufd_device_attach() has already succeeded. @pasid is
 * IOMMU_NO_PASID for no pasid usage.
 */
int iommufd_device_replace(struct iommufd_device *idev, ioasid_t pasid,
			   u32 *pt_id)
{
	return iommufd_device_change_pt(idev, pasid, pt_id,
					&iommufd_device_do_replace);
}
EXPORT_SYMBOL_NS_GPL(iommufd_device_replace, "IOMMUFD");

/**
 * iommufd_device_detach - Disconnect a device/device to an iommu_domain
 * @idev: device to detach
 * @pasid: pasid to detach
 *
 * Undo iommufd_device_attach(). This disconnects the idev from the previously
 * attached pt_id. The device returns back to a blocked DMA translation.
 * @pasid is IOMMU_NO_PASID for no pasid usage.
 */
void iommufd_device_detach(struct iommufd_device *idev, ioasid_t pasid)
{
	struct iommufd_hw_pagetable *hwpt;

	hwpt = iommufd_hw_pagetable_detach(idev, pasid);
	if (!hwpt)
		return;
	iommufd_hw_pagetable_put(idev->ictx, hwpt);
	refcount_dec(&idev->obj.users);
}
EXPORT_SYMBOL_NS_GPL(iommufd_device_detach, "IOMMUFD");

/*
 * On success, it will refcount_inc() at a valid new_ioas and refcount_dec() at
 * a valid cur_ioas (access->ioas). A caller passing in a valid new_ioas should
 * call iommufd_put_object() if it does an iommufd_get_object() for a new_ioas.
 */
static int iommufd_access_change_ioas(struct iommufd_access *access,
				      struct iommufd_ioas *new_ioas)
{
	u32 iopt_access_list_id = access->iopt_access_list_id;
	struct iommufd_ioas *cur_ioas = access->ioas;
	int rc;

	lockdep_assert_held(&access->ioas_lock);

	/* We are racing with a concurrent detach, bail */
	if (cur_ioas != access->ioas_unpin)
		return -EBUSY;

	if (cur_ioas == new_ioas)
		return 0;

	/*
	 * Set ioas to NULL to block any further iommufd_access_pin_pages().
	 * iommufd_access_unpin_pages() can continue using access->ioas_unpin.
	 */
	access->ioas = NULL;

	if (new_ioas) {
		rc = iopt_add_access(&new_ioas->iopt, access);
		if (rc) {
			access->ioas = cur_ioas;
			return rc;
		}
		refcount_inc(&new_ioas->obj.users);
	}

	if (cur_ioas) {
		if (!iommufd_access_is_internal(access) && access->ops->unmap) {
			mutex_unlock(&access->ioas_lock);
			access->ops->unmap(access->data, 0, ULONG_MAX);
			mutex_lock(&access->ioas_lock);
		}
		iopt_remove_access(&cur_ioas->iopt, access, iopt_access_list_id);
		refcount_dec(&cur_ioas->obj.users);
	}

	access->ioas = new_ioas;
	access->ioas_unpin = new_ioas;

	return 0;
}

static int iommufd_access_change_ioas_id(struct iommufd_access *access, u32 id)
{
	struct iommufd_ioas *ioas = iommufd_get_ioas(access->ictx, id);
	int rc;

	if (IS_ERR(ioas))
		return PTR_ERR(ioas);
	rc = iommufd_access_change_ioas(access, ioas);
	iommufd_put_object(access->ictx, &ioas->obj);
	return rc;
}

void iommufd_access_destroy_object(struct iommufd_object *obj)
{
	struct iommufd_access *access =
		container_of(obj, struct iommufd_access, obj);

	mutex_lock(&access->ioas_lock);
	if (access->ioas)
		WARN_ON(iommufd_access_change_ioas(access, NULL));
	mutex_unlock(&access->ioas_lock);
	if (!iommufd_access_is_internal(access))
		iommufd_ctx_put(access->ictx);
}

static struct iommufd_access *__iommufd_access_create(struct iommufd_ctx *ictx)
{
	struct iommufd_access *access;

	/*
	 * There is no uAPI for the access object, but to keep things symmetric
	 * use the object infrastructure anyhow.
	 */
	access = iommufd_object_alloc(ictx, access, IOMMUFD_OBJ_ACCESS);
	if (IS_ERR(access))
		return access;

	/* The calling driver is a user until iommufd_access_destroy() */
	refcount_inc(&access->obj.users);
	mutex_init(&access->ioas_lock);
	return access;
}

struct iommufd_access *iommufd_access_create_internal(struct iommufd_ctx *ictx)
{
	struct iommufd_access *access;

	access = __iommufd_access_create(ictx);
	if (IS_ERR(access))
		return access;
	access->iova_alignment = PAGE_SIZE;

	iommufd_object_finalize(ictx, &access->obj);
	return access;
}

/**
 * iommufd_access_create - Create an iommufd_access
 * @ictx: iommufd file descriptor
 * @ops: Driver's ops to associate with the access
 * @data: Opaque data to pass into ops functions
 * @id: Output ID number to return to userspace for this access
 *
 * An iommufd_access allows a driver to read/write to the IOAS without using
 * DMA. The underlying CPU memory can be accessed using the
 * iommufd_access_pin_pages() or iommufd_access_rw() functions.
 *
 * The provided ops are required to use iommufd_access_pin_pages().
 */
struct iommufd_access *
iommufd_access_create(struct iommufd_ctx *ictx,
		      const struct iommufd_access_ops *ops, void *data, u32 *id)
{
	struct iommufd_access *access;

	access = __iommufd_access_create(ictx);
	if (IS_ERR(access))
		return access;

	access->data = data;
	access->ops = ops;

	if (ops->needs_pin_pages)
		access->iova_alignment = PAGE_SIZE;
	else
		access->iova_alignment = 1;

	access->ictx = ictx;
	iommufd_ctx_get(ictx);
	iommufd_object_finalize(ictx, &access->obj);
	*id = access->obj.id;
	return access;
}
EXPORT_SYMBOL_NS_GPL(iommufd_access_create, "IOMMUFD");

/**
 * iommufd_access_destroy - Destroy an iommufd_access
 * @access: The access to destroy
 *
 * The caller must stop using the access before destroying it.
 */
void iommufd_access_destroy(struct iommufd_access *access)
{
	iommufd_object_destroy_user(access->ictx, &access->obj);
}
EXPORT_SYMBOL_NS_GPL(iommufd_access_destroy, "IOMMUFD");

void iommufd_access_detach(struct iommufd_access *access)
{
	mutex_lock(&access->ioas_lock);
	if (WARN_ON(!access->ioas)) {
		mutex_unlock(&access->ioas_lock);
		return;
	}
	WARN_ON(iommufd_access_change_ioas(access, NULL));
	mutex_unlock(&access->ioas_lock);
}
EXPORT_SYMBOL_NS_GPL(iommufd_access_detach, "IOMMUFD");

int iommufd_access_attach(struct iommufd_access *access, u32 ioas_id)
{
	int rc;

	mutex_lock(&access->ioas_lock);
	if (WARN_ON(access->ioas)) {
		mutex_unlock(&access->ioas_lock);
		return -EINVAL;
	}

	rc = iommufd_access_change_ioas_id(access, ioas_id);
	mutex_unlock(&access->ioas_lock);
	return rc;
}
EXPORT_SYMBOL_NS_GPL(iommufd_access_attach, "IOMMUFD");

int iommufd_access_attach_internal(struct iommufd_access *access,
				   struct iommufd_ioas *ioas)
{
	int rc;

	mutex_lock(&access->ioas_lock);
	if (WARN_ON(access->ioas)) {
		mutex_unlock(&access->ioas_lock);
		return -EINVAL;
	}

	rc = iommufd_access_change_ioas(access, ioas);
	mutex_unlock(&access->ioas_lock);
	return rc;
}

int iommufd_access_replace(struct iommufd_access *access, u32 ioas_id)
{
	int rc;

	mutex_lock(&access->ioas_lock);
	if (!access->ioas) {
		mutex_unlock(&access->ioas_lock);
		return -ENOENT;
	}
	rc = iommufd_access_change_ioas_id(access, ioas_id);
	mutex_unlock(&access->ioas_lock);
	return rc;
}
EXPORT_SYMBOL_NS_GPL(iommufd_access_replace, "IOMMUFD");

/**
 * iommufd_access_notify_unmap - Notify users of an iopt to stop using it
 * @iopt: iopt to work on
 * @iova: Starting iova in the iopt
 * @length: Number of bytes
 *
 * After this function returns there should be no users attached to the pages
 * linked to this iopt that intersect with iova,length. Anyone that has attached
 * a user through iopt_access_pages() needs to detach it through
 * iommufd_access_unpin_pages() before this function returns.
 *
 * iommufd_access_destroy() will wait for any outstanding unmap callback to
 * complete. Once iommufd_access_destroy() no unmap ops are running or will
 * run in the future. Due to this a driver must not create locking that prevents
 * unmap to complete while iommufd_access_destroy() is running.
 */
void iommufd_access_notify_unmap(struct io_pagetable *iopt, unsigned long iova,
				 unsigned long length)
{
	struct iommufd_ioas *ioas =
		container_of(iopt, struct iommufd_ioas, iopt);
	struct iommufd_access *access;
	unsigned long index;

	xa_lock(&ioas->iopt.access_list);
	xa_for_each(&ioas->iopt.access_list, index, access) {
		if (!iommufd_lock_obj(&access->obj) ||
		    iommufd_access_is_internal(access))
			continue;
		xa_unlock(&ioas->iopt.access_list);

		access->ops->unmap(access->data, iova, length);

		iommufd_put_object(access->ictx, &access->obj);
		xa_lock(&ioas->iopt.access_list);
	}
	xa_unlock(&ioas->iopt.access_list);
}

/**
 * iommufd_access_unpin_pages() - Undo iommufd_access_pin_pages
 * @access: IOAS access to act on
 * @iova: Starting IOVA
 * @length: Number of bytes to access
 *
 * Return the struct page's. The caller must stop accessing them before calling
 * this. The iova/length must exactly match the one provided to access_pages.
 */
void iommufd_access_unpin_pages(struct iommufd_access *access,
				unsigned long iova, unsigned long length)
{
	bool internal = iommufd_access_is_internal(access);
	struct iopt_area_contig_iter iter;
	struct io_pagetable *iopt;
	unsigned long last_iova;
	struct iopt_area *area;

	if (WARN_ON(!length) ||
	    WARN_ON(check_add_overflow(iova, length - 1, &last_iova)))
		return;

	mutex_lock(&access->ioas_lock);
	/*
	 * The driver must be doing something wrong if it calls this before an
	 * iommufd_access_attach() or after an iommufd_access_detach().
	 */
	if (WARN_ON(!access->ioas_unpin)) {
		mutex_unlock(&access->ioas_lock);
		return;
	}
	iopt = &access->ioas_unpin->iopt;

	down_read(&iopt->iova_rwsem);
	iopt_for_each_contig_area(&iter, area, iopt, iova, last_iova)
		iopt_area_remove_access(
			area, iopt_area_iova_to_index(area, iter.cur_iova),
			iopt_area_iova_to_index(
				area,
				min(last_iova, iopt_area_last_iova(area))),
			internal);
	WARN_ON(!iopt_area_contig_done(&iter));
	up_read(&iopt->iova_rwsem);
	mutex_unlock(&access->ioas_lock);
}
EXPORT_SYMBOL_NS_GPL(iommufd_access_unpin_pages, "IOMMUFD");

static bool iopt_area_contig_is_aligned(struct iopt_area_contig_iter *iter)
{
	if (iopt_area_start_byte(iter->area, iter->cur_iova) % PAGE_SIZE)
		return false;

	if (!iopt_area_contig_done(iter) &&
	    (iopt_area_start_byte(iter->area, iopt_area_last_iova(iter->area)) %
	     PAGE_SIZE) != (PAGE_SIZE - 1))
		return false;
	return true;
}

static bool check_area_prot(struct iopt_area *area, unsigned int flags)
{
	if (flags & IOMMUFD_ACCESS_RW_WRITE)
		return area->iommu_prot & IOMMU_WRITE;
	return area->iommu_prot & IOMMU_READ;
}

/**
 * iommufd_access_pin_pages() - Return a list of pages under the iova
 * @access: IOAS access to act on
 * @iova: Starting IOVA
 * @length: Number of bytes to access
 * @out_pages: Output page list
 * @flags: IOPMMUFD_ACCESS_RW_* flags
 *
 * Reads @length bytes starting at iova and returns the struct page * pointers.
 * These can be kmap'd by the caller for CPU access.
 *
 * The caller must perform iommufd_access_unpin_pages() when done to balance
 * this.
 *
 * This API always requires a page aligned iova. This happens naturally if the
 * ioas alignment is >= PAGE_SIZE and the iova is PAGE_SIZE aligned. However
 * smaller alignments have corner cases where this API can fail on otherwise
 * aligned iova.
 */
int iommufd_access_pin_pages(struct iommufd_access *access, unsigned long iova,
			     unsigned long length, struct page **out_pages,
			     unsigned int flags)
{
	bool internal = iommufd_access_is_internal(access);
	struct iopt_area_contig_iter iter;
	struct io_pagetable *iopt;
	unsigned long last_iova;
	struct iopt_area *area;
	int rc;

	/* Driver's ops don't support pin_pages */
	if (IS_ENABLED(CONFIG_IOMMUFD_TEST) &&
	    WARN_ON(access->iova_alignment != PAGE_SIZE ||
		    (!internal && !access->ops->unmap)))
		return -EINVAL;

	if (!length)
		return -EINVAL;
	if (check_add_overflow(iova, length - 1, &last_iova))
		return -EOVERFLOW;

	mutex_lock(&access->ioas_lock);
	if (!access->ioas) {
		mutex_unlock(&access->ioas_lock);
		return -ENOENT;
	}
	iopt = &access->ioas->iopt;

	down_read(&iopt->iova_rwsem);
	iopt_for_each_contig_area(&iter, area, iopt, iova, last_iova) {
		unsigned long last = min(last_iova, iopt_area_last_iova(area));
		unsigned long last_index = iopt_area_iova_to_index(area, last);
		unsigned long index =
			iopt_area_iova_to_index(area, iter.cur_iova);

		if (area->prevent_access ||
		    !iopt_area_contig_is_aligned(&iter)) {
			rc = -EINVAL;
			goto err_remove;
		}

		if (!check_area_prot(area, flags)) {
			rc = -EPERM;
			goto err_remove;
		}

		rc = iopt_area_add_access(area, index, last_index, out_pages,
					  flags, internal);
		if (rc)
			goto err_remove;
		out_pages += last_index - index + 1;
	}
	if (!iopt_area_contig_done(&iter)) {
		rc = -ENOENT;
		goto err_remove;
	}

	up_read(&iopt->iova_rwsem);
	mutex_unlock(&access->ioas_lock);
	return 0;

err_remove:
	if (iova < iter.cur_iova) {
		last_iova = iter.cur_iova - 1;
		iopt_for_each_contig_area(&iter, area, iopt, iova, last_iova)
			iopt_area_remove_access(
				area,
				iopt_area_iova_to_index(area, iter.cur_iova),
				iopt_area_iova_to_index(
					area, min(last_iova,
						  iopt_area_last_iova(area))),
				internal);
	}
	up_read(&iopt->iova_rwsem);
	mutex_unlock(&access->ioas_lock);
	return rc;
}
EXPORT_SYMBOL_NS_GPL(iommufd_access_pin_pages, "IOMMUFD");

/**
 * iommufd_access_rw - Read or write data under the iova
 * @access: IOAS access to act on
 * @iova: Starting IOVA
 * @data: Kernel buffer to copy to/from
 * @length: Number of bytes to access
 * @flags: IOMMUFD_ACCESS_RW_* flags
 *
 * Copy kernel to/from data into the range given by IOVA/length. If flags
 * indicates IOMMUFD_ACCESS_RW_KTHREAD then a large copy can be optimized
 * by changing it into copy_to/from_user().
 */
int iommufd_access_rw(struct iommufd_access *access, unsigned long iova,
		      void *data, size_t length, unsigned int flags)
{
	struct iopt_area_contig_iter iter;
	struct io_pagetable *iopt;
	struct iopt_area *area;
	unsigned long last_iova;
	int rc = -EINVAL;

	if (!length)
		return -EINVAL;
	if (check_add_overflow(iova, length - 1, &last_iova))
		return -EOVERFLOW;

	mutex_lock(&access->ioas_lock);
	if (!access->ioas) {
		mutex_unlock(&access->ioas_lock);
		return -ENOENT;
	}
	iopt = &access->ioas->iopt;

	down_read(&iopt->iova_rwsem);
	iopt_for_each_contig_area(&iter, area, iopt, iova, last_iova) {
		unsigned long last = min(last_iova, iopt_area_last_iova(area));
		unsigned long bytes = (last - iter.cur_iova) + 1;

		if (area->prevent_access) {
			rc = -EINVAL;
			goto err_out;
		}

		if (!check_area_prot(area, flags)) {
			rc = -EPERM;
			goto err_out;
		}

		rc = iopt_pages_rw_access(
			area->pages, iopt_area_start_byte(area, iter.cur_iova),
			data, bytes, flags);
		if (rc)
			goto err_out;
		data += bytes;
	}
	if (!iopt_area_contig_done(&iter))
		rc = -ENOENT;
err_out:
	up_read(&iopt->iova_rwsem);
	mutex_unlock(&access->ioas_lock);
	return rc;
}
EXPORT_SYMBOL_NS_GPL(iommufd_access_rw, "IOMMUFD");

int iommufd_get_hw_info(struct iommufd_ucmd *ucmd)
{
	const u32 SUPPORTED_FLAGS = IOMMU_HW_INFO_FLAG_INPUT_TYPE;
	struct iommu_hw_info *cmd = ucmd->cmd;
	void __user *user_ptr = u64_to_user_ptr(cmd->data_uptr);
	const struct iommu_ops *ops;
	struct iommufd_device *idev;
	unsigned int data_len;
	unsigned int copy_len;
	void *data;
	int rc;

	if (cmd->flags & ~SUPPORTED_FLAGS)
		return -EOPNOTSUPP;
	if (cmd->__reserved[0] || cmd->__reserved[1] || cmd->__reserved[2])
		return -EOPNOTSUPP;

	/* Clear the type field since drivers don't support a random input */
	if (!(cmd->flags & IOMMU_HW_INFO_FLAG_INPUT_TYPE))
		cmd->in_data_type = IOMMU_HW_INFO_TYPE_DEFAULT;

	idev = iommufd_get_device(ucmd, cmd->dev_id);
	if (IS_ERR(idev))
		return PTR_ERR(idev);

	ops = dev_iommu_ops(idev->dev);
	if (ops->hw_info) {
		data = ops->hw_info(idev->dev, &data_len, &cmd->out_data_type);
		if (IS_ERR(data)) {
			rc = PTR_ERR(data);
			goto out_put;
		}

		/*
		 * drivers that have hw_info callback should have a unique
		 * iommu_hw_info_type.
		 */
		if (WARN_ON_ONCE(cmd->out_data_type ==
				 IOMMU_HW_INFO_TYPE_NONE)) {
			rc = -EOPNOTSUPP;
			goto out_free;
		}
	} else {
		cmd->out_data_type = IOMMU_HW_INFO_TYPE_NONE;
		data_len = 0;
		data = NULL;
	}

	copy_len = min(cmd->data_len, data_len);
	if (copy_to_user(user_ptr, data, copy_len)) {
		rc = -EFAULT;
		goto out_free;
	}

	/*
	 * Zero the trailing bytes if the user buffer is bigger than the
	 * data size kernel actually has.
	 */
	if (copy_len < cmd->data_len) {
		if (clear_user(user_ptr + copy_len, cmd->data_len - copy_len)) {
			rc = -EFAULT;
			goto out_free;
		}
	}

	/*
	 * We return the length the kernel supports so userspace may know what
	 * the kernel capability is. It could be larger than the input buffer.
	 */
	cmd->data_len = data_len;

	cmd->out_capabilities = 0;
	if (device_iommu_capable(idev->dev, IOMMU_CAP_DIRTY_TRACKING))
		cmd->out_capabilities |= IOMMU_HW_CAP_DIRTY_TRACKING;

	cmd->out_max_pasid_log2 = 0;
	/*
	 * Currently, all iommu drivers enable PASID in the probe_device()
	 * op if iommu and device supports it. So the max_pasids stored in
	 * dev->iommu indicates both PASID support and enable status. A
	 * non-zero dev->iommu->max_pasids means PASID is supported and
	 * enabled. The iommufd only reports PASID capability to userspace
	 * if it's enabled.
	 */
	if (idev->dev->iommu->max_pasids) {
		cmd->out_max_pasid_log2 = ilog2(idev->dev->iommu->max_pasids);

		if (dev_is_pci(idev->dev)) {
			struct pci_dev *pdev = to_pci_dev(idev->dev);
			int ctrl;

			ctrl = pci_pasid_status(pdev);

			WARN_ON_ONCE(ctrl < 0 ||
				     !(ctrl & PCI_PASID_CTRL_ENABLE));

			if (ctrl & PCI_PASID_CTRL_EXEC)
				cmd->out_capabilities |=
						IOMMU_HW_CAP_PCI_PASID_EXEC;
			if (ctrl & PCI_PASID_CTRL_PRIV)
				cmd->out_capabilities |=
						IOMMU_HW_CAP_PCI_PASID_PRIV;
		}
	}

	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));
out_free:
	kfree(data);
out_put:
	iommufd_put_object(ucmd->ictx, &idev->obj);
	return rc;
}
