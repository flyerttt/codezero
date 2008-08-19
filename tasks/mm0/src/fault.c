/*
 * Page fault handling.
 *
 * Copyright (C) 2007, 2008 Bahadir Balban
 */
#include <vm_area.h>
#include <task.h>
#include <mm/alloc_page.h>
#include <kmalloc/kmalloc.h>
#include <l4lib/arch/syscalls.h>
#include <l4lib/arch/syslib.h>
#include INC_GLUE(memory.h)
#include INC_SUBARCH(mm.h)
#include <arch/mm.h>
#include <l4/generic/space.h>
#include <l4/api/errno.h>
#include <string.h>
#include <memory.h>
#include <shm.h>
#include <file.h>

/* Given a page and the vma it is in, returns that page's virtual address */
unsigned long vma_page_to_virtual(struct vm_area *vma, struct page *p)
{
	return __pfn_to_addr(vma->pfn_start + p->offset);
}

unsigned long fault_to_file_offset(struct fault_data *fault)
{
	/* Fault's offset in its vma */
	unsigned long vma_off_pfn = __pfn(fault->address) - fault->vma->pfn_start;

	/* Fault's offset in the file */
	unsigned long f_off_pfn = fault->vma->file_offset + vma_off_pfn;

	return f_off_pfn;
}

/*
 * Given a reference to a vm_object link, returns the next link but
 * avoids wrapping around back to head. If next is head, returns 0.
 *
 * vma->link1->link2->link3
 *       |      |      |
 *       V      V      V
 *       vmo1   vmo2   vmo3|vm_file
 *
 * Example:
 * Given a reference to link = vma, head = vma, returns link1.
 * Given a reference to link = link3, head = vma, returns 0.
 */
struct vm_obj_link *vma_next_link(struct list_head *link,
				  struct list_head *head)
{
	BUG_ON(list_empty(link));
	if (link->next == head)
		return 0;
	else
		return list_entry(link->next, struct vm_obj_link, list);
}

/* Unlinks orig_link from its vma and deletes it but keeps the object. */
int vma_drop_link(struct vm_obj_link *shadower_link,
		  struct vm_obj_link *orig_link)
{
	/* Remove object link from vma's list */
	list_del(&orig_link->list);

	/* Reduce object's ref count */
	orig_link->obj->refcnt--;

	/*
	 * Refcount could go as low as 1 but not zero because shortly
	 * after it goes down to one, it is removed from the link
	 * chain so it can never exist with a refcount less than 1
	 * in the chain.
	 */
	if (orig_link->obj->refcnt < 1) {
		printf("%s: Shadower:\n", __FUNCTION__);
		vm_object_print(shadower_link->obj);

		printf("%s: Original:\n", __FUNCTION__);
		vm_object_print(orig_link->obj);
		BUG();
	}

	/*
	 * Remove the shadower from original's shadower list.
	 * We know shadower is deleted from original's list
	 * because each shadow can shadow a single object.
	 */
	list_del(&shadower_link->shref);

	/* Delete the original link */
	kfree(orig_link);

	return 0;
}

/*
 * Checks if page cache pages of lesser is a subset of those of copier.
 * Note this just checks the page cache, so if any objects have pages
 * swapped to disk, this function does not rule.
 */
int vm_object_is_subset(struct vm_object *copier,
			struct vm_object *lesser)
{
	struct page *pc, *pl;

	/* Copier must have equal or more pages to overlap lesser */
	if (copier->npages < lesser->npages)
		return 0;

	/*
	 * Do a page by page comparison. Every lesser page
	 * must be in copier for overlap.
	 */
	list_for_each_entry(pl, &lesser->page_cache, list)
		if (!(pc = find_page(copier, pl->offset)))
			return 0;
	/*
	 * For all pages of lesser vmo, there seems to be a page
	 * in the copier vmo. So lesser is a subset of copier
	 */
	return 1;
}

/* Merges link 1 to link 2 */
int vma_do_merge_link(struct vm_obj_link *link1, struct vm_obj_link *link2)
{
	struct vm_object *obj1 = link1->obj;
	struct vm_object *obj2 = link2->obj;
	struct page *p1, *p2;

	/* Move all non-intersecting pages to link2. */
	list_for_each_entry(p1, &obj1->page_cache, list) {
		/* Page doesn't exist, move it to shadower */
		if (!(p2 = find_page(obj2, p1->offset))) {
			list_del(&p1->list);
			spin_lock(&p1->lock);
			p1->owner = obj2;
			spin_unlock(&p1->lock);
			insert_page_olist(p1, obj2);
			obj2->npages++;
		}
	}
	/* Delete the object along with all its pages. */
	vm_object_delete(obj1);

	/* Delete the last link for the object */
	kfree(link1);

	return 0;
}

/*
 * Finds the only shadower of a vm object, finds the link to
 * the object that is in the same vma as the shadower, and
 * merges the two in the shadower object, frees the link and
 * the original object. Note this must be called when merge
 * is decided.
 */
int vma_merge_link(struct vm_object *vmo)
{
	struct vm_obj_link *sh_link, *vmo_link;

	/* Check refcount */
	BUG_ON(vmo->refcnt != 1);

	/* Get the last shadower entry */
	sh_link = list_entry(vmo->shadowers.next,
			     struct vm_obj_link, shref);

	/* Remove it from original's shadow list */
	list_del(&sh_link->shref);

	/* Check that there really was one shadower left */
	BUG_ON(!list_empty(&vmo_link->obj->shadowers));

	/*
	 * Get the link to vmo that is in the same list as
	 * the only shadower
	 */
	vmo_link = list_entry(sh_link->list.next,
			      struct vm_obj_link, list);

	/*
	 * Check that we got the right link. Since it is
	 * an ordered list, the link must be the following
	 * entry after its shadower.
	 */
	BUG_ON(vmo_link->obj != vmo);

	/*
	 * Now that we got the consecutive links in the
	 * same vma, do the actual merge.
	 */
	vma_do_merge_link(vmo_link, sh_link);

	return 0;
}

struct vm_obj_link *vm_objlink_create(void)
{
	struct vm_obj_link *vmo_link;

	if (!(vmo_link = kzalloc(sizeof(*vmo_link))))
		return PTR_ERR(-ENOMEM);
	INIT_LIST_HEAD(&vmo_link->list);
	INIT_LIST_HEAD(&vmo_link->shref);

	return vmo_link;
}

/*
 * Creates a bare vm_object along with its vma link, since
 * the shadow will be immediately used in a vma object list.
 */
struct vm_obj_link *vma_create_shadow(void)
{
	struct vm_object *vmo;
	struct vm_obj_link *vmo_link;

	/* FIXME: Why not use vm_objlink_create() ??? */
	if (!(vmo_link = kzalloc(sizeof(*vmo_link))))
		return 0;

	if (!(vmo = vm_object_create())) {
		kfree(vmo_link);
		return 0;
	}
	INIT_LIST_HEAD(&vmo_link->list);
	INIT_LIST_HEAD(&vmo_link->shref);
	vmo->flags = VM_OBJ_SHADOW;
	vmo_link->obj = vmo;

	return vmo_link;
}

/* Allocates a new page, copies the original onto it and returns. */
struct page *copy_to_new_page(struct page *orig)
{
	void *new_vaddr, *vaddr, *paddr;
	struct page *new;

	if (!(paddr = alloc_page(1)))
		return 0;

	new = phys_to_page(paddr);

	/* Map the new and orig page to self */
	new_vaddr = l4_map_helper(paddr, 1);
	vaddr = l4_map_helper((void *)page_to_phys(orig), 1);

	/* Copy the page into new page */
	memcpy(new_vaddr, vaddr, PAGE_SIZE);

	/* Unmap both pages from current task. */
	l4_unmap_helper(vaddr, 1);
	l4_unmap_helper(new_vaddr, 1);

	return new;
}

/* TODO:
 * - Why not allocate a swap descriptor in vma_create_shadow() rather than
 *   a bare vm_object? It will be needed.
 * - Check refcounting of shadows, their references, page refs,
 *   reduces increases etc.
 *
 *   This handles copy-on-write semantics in various situations. Returns
 *   page struct for copy page availabe for mapping.
 *
 *   1) Copy-on-write of read-only files. (Creates r/w shadows/adds pages)
 *   2) Copy-on-write of forked RO shadows (Creates r/w shadows/adds pages)
 *   3) Copy-on-write of shm files. (Adds pages to r/w shm file from devzero).
 */
struct page *copy_on_write(struct fault_data *fault)
{
	struct vm_obj_link *vmo_link, *shadow_link, *copier_link;
	struct vm_object *vmo, *shadow;
	struct page *page, *new_page;
	struct vm_area *vma = fault->vma;
	unsigned long file_offset = fault_to_file_offset(fault);

	/* Get the first object, either original file or a shadow */
	if (!(vmo_link = vma_next_link(&vma->vm_obj_list, &vma->vm_obj_list))) {
		printf("%s:%s: No vm object in vma!\n",
		       __TASKNAME__, __FUNCTION__);
		BUG();
	}

	/* Is the object read-only? Create a shadow object if so.
	 *
	 * NOTE: Whenever the topmost object is read-only, a new shadow
	 * object must be created. When there are no shadows one is created
	 * because, its the original vm_object that is not writeable, and
	 * when there are shadows one is created because a fork had just
	 * happened, in which case all shadows are rendered read-only.
	 */
	if (!(vmo_link->obj->flags & VM_WRITE)) {
		if (!(shadow_link = vma_create_shadow()))
			return PTR_ERR(-ENOMEM);
		dprintf("%s: Created a shadow.\n", __TASKNAME__);
		/* Initialise the shadow */
		shadow = shadow_link->obj;
		shadow->refcnt = 1;
		shadow->orig_obj = vmo_link->obj;
		shadow->flags = VM_OBJ_SHADOW | VM_WRITE;
		shadow->pager = &swap_pager;

		/*
		 * Add the shadow in front of the original:
		 *
 		 * vma->link0->link1
 		 *       |      |
 		 *       v      v
 		 *       shadow original
		 */
		list_add(&shadow_link->list, &vma->vm_obj_list);

		/* Add to global object list */
		list_add(&shadow->list, &vm_object_list);

		/* Shadow is the copier object */
		copier_link = shadow_link;
	} else {
		dprintf("No new shadows. Going to add to "
			"topmost r/w shadow object\n");
		/* No new shadows, the topmost r/w vmo is the copier object */
		copier_link = vmo_link;

		/*
		 * We start page search on read-only objects. If the first
		 * one was writable, go to next which must be read-only.
		 */
		BUG_ON(!(vmo_link = vma_next_link(&vmo_link->list,
						  &vma->vm_obj_list)));
		BUG_ON(vmo_link->obj->flags & VM_WRITE);
	}

	/* Traverse the list of read-only vm objects and search for the page */
	while (IS_ERR(page = vmo_link->obj->pager->ops.page_in(vmo_link->obj,
							       file_offset))) {
		if (!(vmo_link = vma_next_link(&vmo_link->list,
					       &vma->vm_obj_list))) {
			printf("%s:%s: Traversed all shadows and the original "
			       "file's vm_object, but could not find the "
			       "faulty page in this vma.\n",__TASKNAME__,
			       __FUNCTION__);
			BUG();
		}
	}

	/*
	 * Copy the page. This traverse and copy is like a page-in operation
	 * of a pager, except that the page is moving along vm_objects.
	 */
	new_page = copy_to_new_page(page);

	/* Update page details */
	spin_lock(&new_page->lock);
	new_page->refcnt = 0;
	new_page->owner = copier_link->obj;
	new_page->offset = file_offset;
	new_page->virtual = 0;
	BUG_ON(!list_empty(&new_page->list));
	spin_unlock(&page->lock);

	/* Add the page to owner's list of in-memory pages */
	insert_page_olist(new_page, new_page->owner);
	new_page->owner->npages++;

	/* Shared faults don't have shadows so we don't look for collapses */
	if (!(vma->flags & VMA_SHARED)) {

		/*
		 * Finished handling the actual fault, now check for possible
		 * shadow collapses. Does the copier completely shadow the one
		 * underlying it?
		 */
		if (!(vmo_link = vma_next_link(&copier_link->list,
					       &vma->vm_obj_list))) {
			/* Copier must have an object under it */
			printf("Copier must have had an object under it!\n");
			BUG();
		}

		/* Compare whether page caches overlap */
		if (vm_object_is_subset(copier_link->obj, vmo_link->obj)) {
			/*
			 * They do overlap, so keep reference to object but
			 * drop and delete the vma link.
			 */
			vmo = vmo_link->obj;
			vma_drop_link(copier_link, vmo_link);
			vmo_link = 0;

			/*
			 * vm object reference down to one
			 * and object is mergeable?
			 */
			if ((vmo->refcnt == 1) &&
			    (vmo->flags != VM_OBJ_FILE))
				vma_merge_link(vmo);
		}
	}

	return new_page;
}


/*
 * Handles the page fault, all entries here are assumed *legal*
 * faults, i.e. do_page_fault() should have already checked
 * for illegal accesses.
 *
 * NOTE:
 * Anon/Shared pages:
 * First access from first process is COW. All subsequent RW
 * accesses (which are attempts of *sharing*) simply map that
 * page to faulting processes.
 *
 * Non-anon/shared pages:
 * First access from first process simply writes to the pages
 * of that file. All subsequent accesses by other processes
 * do so as well.
 */

int __do_page_fault(struct fault_data *fault)
{
	unsigned int reason = fault->reason;
	unsigned int vma_flags = fault->vma->flags;
	unsigned int pte_flags = fault->pte_flags;
	struct vm_area *vma = fault->vma;
	struct vm_obj_link *vmo_link;
	unsigned long file_offset;
	struct page *page;

	/* Handle read */
	if ((reason & VM_READ) && (pte_flags & VM_NONE)) {
		file_offset = fault_to_file_offset(fault);

		/* Get the first object, either original file or a shadow */
		if (!(vmo_link = vma_next_link(&vma->vm_obj_list, &vma->vm_obj_list))) {
			printf("%s:%s: No vm object in vma!\n",
			       __TASKNAME__, __FUNCTION__);
			BUG();
		}

		/* Traverse the list of read-only vm objects and search for the page */
		while (IS_ERR(page = vmo_link->obj->pager->ops.page_in(vmo_link->obj,
								       file_offset))) {
			if (!(vmo_link = vma_next_link(&vmo_link->list,
						       &vma->vm_obj_list))) {
				printf("%s:%s: Traversed all shadows and the original "
				       "file's vm_object, but could not find the "
				       "faulty page in this vma.\n",__TASKNAME__,
				       __FUNCTION__);
				BUG();
			}
		}
		BUG_ON(!page);
	}

	/* Handle write */
	if ((reason & VM_WRITE) && (pte_flags & VM_READ)) {
		/* Copy-on-write. All private vmas are always COW */
		if (vma_flags & VMA_PRIVATE) {
			BUG_ON(IS_ERR(page = copy_on_write(fault)));

		/*
		 * This handles shared pages that are both anon and non-anon.
		 */
		} else if ((vma_flags & VMA_SHARED)) {
			file_offset = fault_to_file_offset(fault);
			BUG_ON(!(vmo_link = vma_next_link(&vma->vm_obj_list,
							  &vma->vm_obj_list)));

			/* Get the page from its pager */
			if (IS_ERR(page = vmo_link->obj->pager->ops.page_in(vmo_link->obj,
									    file_offset))) {
				/*
				 * Writable page does not exist,
				 * if it is anonymous, it needs to be COW'ed,
				 * otherwise the file must have paged-in this
				 * page, so its a bug.
				 */
				if (vma_flags & VMA_ANONYMOUS) {
					BUG_ON(IS_ERR(page = copy_on_write(fault)));
					goto out_success;
				} else {
					printf("%s: Could not obtain faulty "
					       "page from regular file.\n",
					       __TASKNAME__);
					BUG();
				}
			}
			BUG_ON(!page);
		} else
			BUG();
	}

out_success:
	/* Map the new page to faulty task */
	l4_map((void *)page_to_phys(page),
	       (void *)page_align(fault->address), 1,
	       (reason & VM_READ) ? MAP_USR_RO_FLAGS : MAP_USR_RW_FLAGS,
	       fault->task->tid);
	dprintf("%s: Mapped 0x%x as writable to tid %d.\n", __TASKNAME__,
		page_align(fault->address), fault->task->tid);
	vm_object_print(page->owner);

	return 0;
}

/*
 * Sets all r/w shadow objects as read-only for the process
 * so that as expected after a fork() operation, writes to those
 * objects cause copy-on-write incidents.
 */
int vm_freeze_shadows(struct tcb *task)
{
	unsigned long virtual;
	struct vm_area *vma;
	struct vm_obj_link *vmo_link;
	struct vm_object *vmo;
	struct page *p;

	list_for_each_entry(vma, &task->vm_area_list, list) {

		/* Shared vmas don't have shadows */
		if (vma->flags & VMA_SHARED)
			continue;

		/* Get the first object */
		while ((vmo_link = vma_next_link(&vma->vm_obj_list,
					 	 &vma->vm_obj_list))) {
			vmo = vmo_link->obj;

			/* Is this a writeable shadow? */
			if ((vmo->flags & VM_OBJ_SHADOW) &&
			    (vmo->flags & VM_WRITE)) {

				/* Make the object read only */
				vmo->flags &= ~VM_WRITE;
				vmo->flags |= VM_READ;

				/*
				 * Make all pages on it read-only
				 * in the page tables.
				 */
				list_for_each_entry(p, &vmo->page_cache, list) {

					/* Find virtual address of each page */
					virtual = vma_page_to_virtual(vma, p);

					/* Map the page as read-only */
					l4_map((void *)page_to_phys(p),
					       (void *)virtual,
					       MAP_USR_RO_FLAGS, task->tid);
				}
				break;
			}
		}
	}

	return 0;
}

#if 0
/*
 * Old function, likely to be ditched.
 *
 * For copy-on-write vmas, grows an existing shadow vma, or creates a new one
 * for the copy-on-write'ed page. Then adds this shadow vma to the actual vma's
 * shadow list. Shadow vmas never overlap with each other, and always overlap
 * with part of their original vma.
 */
struct vm_area *copy_on_write_vma(struct fault_data *fault)
{
	struct vm_area *shadow;
	unsigned long faulty_pfn = __pfn(fault->address);

	BUG_ON(faulty_pfn < fault->vma->pfn_start ||
	       faulty_pfn >= fault->vma->pfn_end);
	list_for_each_entry(shadow, &fault->vma->shadow_list, shadow_list) {
		if (faulty_pfn == (shadow->pfn_start - 1)) {
			/* Growing start of existing shadow vma */
			shadow->pfn_start = faulty_pfn;
			shadow->f_offset -= 1;
			return shadow;
		} else if (faulty_pfn == (shadow->pfn_end + 1)) {
			/* Growing end of existing shadow vma */
			shadow->pfn_end = faulty_pfn;
			return shadow;
		}
	}
	/* Otherwise this is a new shadow vma that must be initialised */
	shadow = kzalloc(sizeof(struct vm_area));
	BUG();	/* This f_offset is wrong. Using uninitialised fields, besides
		   swap offsets calculate differently */
	shadow->f_offset = faulty_pfn - shadow->pfn_start
			   + shadow->f_offset;
	shadow->pfn_start = faulty_pfn;
	shadow->pfn_end = faulty_pfn + 1; /* End pfn is exclusive */
	shadow->flags = fault->vma->flags;

	/* The vma is owned by the swap file, since it's a private vma */
	shadow->owner = fault->task->swap_file;
	INIT_LIST_HEAD(&shadow->list);
	INIT_LIST_HEAD(&shadow->shadow_list);

	/*
	 * The actual vma uses its shadow_list as the list head for shadows.
	 * The shadows use their list member, and shadow_list is unused.
	 */
	list_add(&shadow->list,	&fault->vma->shadow_list);
	return shadow;
}

/*
 * Handles any page ownership change or allocation for file-backed pages.
 */
int do_file_page(struct fault_data *fault)
{
	unsigned int reason = fault->reason;
	unsigned int vma_flags = fault->vma->flags;
	unsigned int pte_flags = vm_prot_flags(fault->kdata->pte);

	/* For RO or non-cow WR pages just read in the page */
	if (((reason & VM_READ) || ((reason & VM_WRITE) && !(vma_flags & VMA_COW)))
	    && (pte_flags & VM_NONE)) {
		/* Allocate a new page */
		void *paddr = alloc_page(1);
		void *vaddr = phys_to_virt(paddr);
		struct page *page = phys_to_page(paddr);
		unsigned long f_offset = fault_to_file_offset(fault);

		/* Map new page at a self virtual address temporarily */
		l4_map(paddr, vaddr, 1, MAP_USR_RW_FLAGS, self_tid());

		/*
		 * Read the page. (Simply read into the faulty area that's
		 * now mapped using a newly allocated page.)
		 */
		if (fault->vma->owner->pager->ops.read_page(fault->vma->owner,
							    f_offset,
							    vaddr) < 0)
			BUG();

		/* Remove temporary mapping */
		l4_unmap(vaddr, 1, self_tid());

		/* Map it to task. */
		l4_map(paddr, (void *)page_align(fault->address), 1,
		       (reason & VM_READ) ? MAP_USR_RO_FLAGS : MAP_USR_RW_FLAGS,
		       fault->task->tid);

		spin_lock(&page->lock);

		/* Update its page descriptor */
		page->count++;
		page->owner = fault->vma->owner;
		page->f_offset = __pfn(fault->address)
				 - fault->vma->pfn_start + fault->vma->f_offset;
		page->virtual = page_align(fault->address);

		/* Add the page to it's owner's list of in-memory pages */
		BUG_ON(!list_empty(&page->list));
		insert_page_olist(page, page->owner);
		spin_unlock(&page->lock);
		//printf("%s: Mapped new page @ 0x%x to task: %d\n", __TASKNAME__,
		//       fault->address, fault->task->tid);
	/* Upgrade RO page to non-cow write */
	} else if ((reason & VM_WRITE) && (pte_flags & VM_READ)
		   && !(vma_flags & VMA_COW)) {
		/* The page is mapped in, just update its permission */
		l4_map((void *)__pte_to_addr(fault->kdata->pte),
		       (void *)page_align(fault->address), 1,
		       MAP_USR_RW_FLAGS, fault->task->tid);

	/*
	 * For cow-write, allocate private pages and create shadow vmas.
	 */
	} else if ((reason & VM_WRITE) && (pte_flags & VM_READ)
		   && (vma_flags & VMA_COW)) {
		void *pa = (void *)__pte_to_addr(fault->kdata->pte);
		void *new_pa = alloc_page(1);
		struct page *page = phys_to_page(pa);
		struct page *new_page = phys_to_page(new_pa);
		void *va, *new_va;

		/* Create or obtain existing shadow vma for the page */
		struct vm_area *shadow = copy_on_write_vma(fault);

		/* Map new page at a local virtual address temporarily */
		new_va = l4_map_helper(new_pa, 1);

		/* Map the old page (vmapped for process but not us) to self */
		va = l4_map_helper(pa, 1);

		/* Copy data from old to new page */
		memcpy(new_va, va, PAGE_SIZE);

		/* Remove temporary mappings */
		l4_unmap(va, 1, self_tid());
		l4_unmap(new_va, 1, self_tid());

		spin_lock(&page->lock);

		/* Clear usage details for original page. */
		page->count--;
		page->virtual = 0; /* FIXME: Maybe mapped for multiple processes ? */

		/* New page is owned by shadow's owner (swap) */
		new_page->owner = shadow->owner;
		new_page->count++;
		new_page->f_offset = __pfn(fault->address)
				     - shadow->pfn_start + shadow->f_offset;
		new_page->virtual = page_align(fault->address);

		/* Add the page to it's owner's list of in-memory pages */
		BUG_ON(!list_empty(&page->list));
		insert_page_olist(page, page->owner);
		spin_unlock(&page->lock);

		/*
		 * Overwrite the original file-backed page's mapping on this
		 * task with the writeable private page. The original physical
		 * page still exists in memory and can be referenced from its
		 * associated owner file, but it's not mapped into any virtual
		 * address anymore in this task.
		 */
		l4_map(new_pa, (void *)page_align(fault->address), 1,
		       MAP_USR_RW_FLAGS, fault->task->tid);

	} else if ((reason & VM_WRITE) && (pte_flags & VM_NONE)
		   && (vma_flags & VMA_COW)) {
		struct vm_area *shadow;

		/* Allocate a new page */
		void *paddr = alloc_page(1);
		void *vaddr = phys_to_virt(paddr);
		struct page *page = phys_to_page(paddr);
		unsigned long f_offset = fault_to_file_offset(fault);

		/* Map it to self */
		l4_map(paddr, vaddr, 1, MAP_USR_RW_FLAGS, self_tid());

		/* Update its page descriptor */
		page->count++;
		page->owner = fault->vma->owner;
		page->f_offset = __pfn(fault->address)
				 - fault->vma->pfn_start + fault->vma->f_offset;
		page->virtual = page_align(fault->address);

		/*
		 * Read the page. (Simply read into the faulty area that's
		 * now mapped using a newly allocated page.)
		 */
		if (fault->vma->owner->pager->ops.read_page(fault->vma->owner,
							    f_offset,
							    vaddr) < 0)
			BUG();

		/* Unmap from self */
		l4_unmap(vaddr, 1, self_tid());

		/* Map to task. */
		l4_map(paddr, (void *)page_align(fault->address), 1,
		       MAP_USR_RW_FLAGS, fault->task->tid);

		/* Obtain a shadow vma for the page */
		shadow = copy_on_write_vma(fault);
		spin_lock(&page->lock);

		/* Now anonymise the page by changing its owner file to swap */
		page->owner = shadow->owner;

		/* Page's offset is different in its new owner. */
		page->f_offset = __pfn(fault->address)
				 - fault->vma->pfn_start + fault->vma->f_offset;

		/* Add the page to it's owner's list of in-memory pages */
		BUG_ON(!list_empty(&page->list));
		insert_page_olist(page, page->owner);
		spin_unlock(&page->lock);
	} else
		BUG();

	return 0;
}

/*
 * Handles any page allocation or file ownership change for anonymous pages.
 * For read accesses initialises a wired-in zero page and for write accesses
 * initialises a private ZI page giving its ownership to the swap file.
 */
int do_anon_page(struct fault_data *fault)
{
	unsigned int pte_flags = vm_prot_flags(fault->kdata->pte);
	void *paddr, *vaddr;
	struct page *page;

	/* If swapped, read in with vma's pager (swap in anon case) */
	if (pte_flags & VM_SWAPPED) {
		BUG();
		// Properly implement:
		// fault->vma->owner->pager->ops.read_page(fault);

		/* Map the page with right permission */
		if (fault->reason & VM_READ)
			l4_map(paddr, (void *)page_align(fault->address), 1,
			       MAP_USR_RO_FLAGS, fault->task->tid);
		else if (fault->reason & VM_WRITE)
			l4_map(paddr, (void *)page_align(fault->address), 1,
			       MAP_USR_RW_FLAGS, fault->task->tid);
		else
			BUG();
		return 0;
	}

	/* For non-existant pages just map the zero page, unless it is the
	 * beginning of stack which requires environment and argument data. */
	if (fault->reason & VM_READ) {
		/*
		 * Zero page is a special wired-in page that is mapped
		 * many times in many tasks. Just update its count field.
		 */
		paddr = get_zero_page();

		l4_map(paddr, (void *)page_align(fault->address), 1,
		       MAP_USR_RO_FLAGS, fault->task->tid);
	}

	/* Write faults require a real zero initialised page */
	if (fault->reason & VM_WRITE) {
		paddr = alloc_page(1);
		vaddr = phys_to_virt(paddr);
		page = phys_to_page(paddr);

		/* NOTE:
		 * This mapping overwrites the original RO mapping which
		 * is anticipated to be the zero page.
		 */
		BUG_ON(__pte_to_addr(fault->kdata->pte) !=
		       (unsigned long)get_zero_page());

		/* Map new page at a self virtual address temporarily */
		l4_map(paddr, vaddr, 1, MAP_USR_RW_FLAGS, self_tid());

		/* Clear the page */
		memset((void *)vaddr, 0, PAGE_SIZE);

		/* Remove temporary mapping */
		l4_unmap((void *)vaddr, 1, self_tid());

		/* Map the page to task */
		l4_map(paddr, (void *)page_align(fault->address), 1,
		       MAP_USR_RW_FLAGS, fault->task->tid);

		/*** DEBUG CODE FOR FS0 UTCB ***/
		if(page_align(fault->address) == 0xf8001000) {
			printf("For FS0 utcb @ 0xf8001000, mapping page @ 0x%x, foffset: 0x%x, owned by vma @ 0x%x, vmfile @ 0x%x\n",
				(unsigned long)page, page->f_offset, fault->vma, fault->vma->owner);
		}
		if(page_align(fault->address) == 0xf8002000) {
			printf("For FS0 utcb @ 0xf8002000, mapping page @ 0x%x, foffset: 0x%x, owned by vma @ 0x%x, vmfile @ 0x%x\n",
				(unsigned long)page, page->f_offset, fault->vma, fault->vma->owner);
		}
		/*** DEBUG CODE FOR FS0 UTCB ***/

		spin_lock(&page->lock);
		/* vma's swap file owns this page */
		page->owner = fault->vma->owner;

		/* Add the page to it's owner's list of in-memory pages */
		BUG_ON(!list_empty(&page->list));
		insert_page_olist(page, page->owner);

		/* The offset of this page in its owner file */
		page->f_offset = __pfn(fault->address)
				 - fault->vma->pfn_start + fault->vma->f_offset;
		page->count++;
		page->virtual = page_align(fault->address);
		spin_unlock(&page->lock);
	}
	return 0;
}
#endif

/*
 * Page fault model:
 *
 * A page is anonymous (e.g. stack)
 *  - page needs read access:
 *  	action: map the zero page.
 *  - page needs write access:
 *      action: allocate ZI page and map that. Swap file owns the page.
 *  - page is swapped to swap:
 *      action: read back from swap file into new page.
 *
 * A page is file-backed but private (e.g. .data section)
 *  - page needs read access:
 *      action: read the page from its file.
 *  - page is swapped out before being private. (i.e. invalidated)
 *      action: read the page from its file. (original file)
 *  - page is swapped out after being private.
 *      action: read the page from its file. (swap file)
 *  - page needs write access:
 *      action: allocate new page, declare page as private, change its
 *              owner to swap file.
 *
 * A page is file backed but not-private, and read-only. (e.g. .text section)
 *  - page needs read access:
 *     action: read in the page from its file.
 *  - page is swapped out. (i.e. invalidated)
 *     action: read in the page from its file.
 *  - page needs write access:
 *     action: forbidden, kill task?
 *
 * A page is file backed but not-private, and read/write. (e.g. any data file.)
 *  - page needs read access:
 *     action: read in the page from its file.
 *  - page is flushed back to its original file. (i.e. instead of swap)
 *     action: read in the page from its file.
 *  - page needs write access:
 *     action: read the page in, give write access.
 */
int do_page_fault(struct fault_data *fault)
{
	unsigned int vma_flags = (fault->vma) ? fault->vma->flags : VM_NONE;
	unsigned int reason = fault->reason;

	/* vma flags show no access */
	if (vma_flags & VM_NONE) {
		printf("Illegal access, tid: %d, address: 0x%x, PC @ 0x%x,\n",
		       fault->task->tid, fault->address, fault->kdata->faulty_pc);
		BUG();
	}

	/* The access reason is not included in the vma's listed flags */
	if (!(reason & vma_flags)) {
		printf("Illegal access, tid: %d, address: 0x%x, PC @ 0x%x\n",
		       fault->task->tid, fault->address, fault->kdata->faulty_pc);
		BUG();
	}

	if ((reason & VM_EXEC) && (vma_flags & VM_EXEC)) {
		printf("Exec faults unsupported yet.\n");
		BUG();	/* Can't handle this yet. */
	}

	/* Handle legitimate faults */
	return __do_page_fault(fault);
}

int page_fault_handler(l4id_t sender, fault_kdata_t *fkdata)
{
	int err;
	struct fault_data fault = {
		/* Fault data from kernel */
		.kdata = fkdata,
	};

	BUG_ON(sender == 0);

	/* Get pager specific task info */
	BUG_ON(!(fault.task = find_task(sender)));

	/* Extract fault reason, fault address etc. in generic format */
	set_generic_fault_params(&fault);

	/* Get vma info */
	if (!(fault.vma = find_vma(fault.address,
				   &fault.task->vm_area_list)))
		printf("Hmm. No vma for faulty region. "
		       "Bad things will happen.\n");

	/* Handle the actual fault */
	err = do_page_fault(&fault);

	/*
	 * Return the ipc and by doing so restart the faulty thread.
	 * FIXME: We could kill the thread if there was an error???
	 * Perhaps via a kill message to kernel?
	 */
	l4_ipc_return(err);
	return 0;
}

/* Checks if an address range is a validly mapped area for a task */
int validate_task_range(struct tcb *t, unsigned long start,
			unsigned long end, unsigned int vmflags)
{
	struct vm_area *vma;

	start = page_align(start);
	end = page_align_up(end);

	/* Find the vma that maps that virtual address */
	for (unsigned long vaddr = start; vaddr < end; vaddr += PAGE_SIZE) {
		if (!(vma = find_vma(vaddr, &t->vm_area_list))) {
			printf("%s: No VMA found for 0x%x on task: %d\n",
			       __FUNCTION__, vaddr, t->tid);
			return -EINVAL;
		}
		if ((vma->flags & vmflags) != vmflags)
			return -EINVAL;
	}
	return 0;
}

/*
 * Makes the virtual to page translation for a given user task.
 * It traverses the vm_objects and returns the first encountered
 * instance of the page. If page is not mapped in the task's address
 * space, (not faulted at all), returns error.
 */
struct page *task_virt_to_page(struct tcb *t, unsigned long virtual)
{
	unsigned long vma_offset;
	unsigned long file_offset;
	struct vm_obj_link *vmo_link;
	struct vm_area *vma;
	struct page *page;

	/* First find the vma that maps that virtual address */
	if (!(vma = find_vma(virtual, &t->vm_area_list))) {
		printf("%s: No VMA found for 0x%x on task: %d\n",
		       __FUNCTION__, virtual, t->tid);
		return PTR_ERR(-EINVAL);
	}

	/* Find the pfn offset of virtual address in this vma */
	BUG_ON(__pfn(virtual) < vma->pfn_start ||
	       __pfn(virtual) > vma->pfn_end);
	vma_offset = __pfn(virtual) - vma->pfn_start;

	/* Find the file offset of virtual address in this file */
	file_offset = vma->file_offset + vma_offset;

	/* Get the first object, either original file or a shadow */
	if (!(vmo_link = vma_next_link(&vma->vm_obj_list, &vma->vm_obj_list))) {
		printf("%s:%s: No vm object in vma!\n",
		       __TASKNAME__, __FUNCTION__);
		BUG();
	}

	/* Traverse the list of read-only vm objects and search for the page */
	while (IS_ERR(page = vmo_link->obj->pager->ops.page_in(vmo_link->obj,
							       file_offset))) {
		if (!(vmo_link = vma_next_link(&vmo_link->list,
					       &vma->vm_obj_list))) {
			printf("%s:%s: Traversed all shadows and the original "
			       "file's vm_object, but could not find the "
			       "faulty page in this vma.\n",__TASKNAME__,
			       __FUNCTION__);
			BUG();
		}
	}

	/* Found it */
	// printf("%s: %s: Found page with file_offset: 0x%x\n",
	//       __TASKNAME__,  __FUNCTION__, page->offset);
	vm_object_print(vmo_link->obj);

	return page;
}

/*
 * Prefaults the page with given virtual address, to given task
 * with given reasons. Multiple reasons are allowed, they are
 * handled separately in order.
 */
int prefault_page(struct tcb *task, unsigned long address,
		  unsigned int vmflags)
{
	int err;
	struct fault_data fault = {
		.task = task,
		.address = address,
	};

	dprintf("Pre-faulting address 0x%x, on task %d, with flags: 0x%x\n",
		address, task->tid, vmflags);

	/* Find the vma */
	if (!(fault.vma = find_vma(fault.address,
				   &fault.task->vm_area_list))) {
		err = -EINVAL;
		dprintf("%s: Invalid: No vma for given address. %d\n",
			__FUNCTION__, err);
		return err;
	}

	/* Flags may indicate multiple fault reasons. First do the read */
	if (vmflags & VM_READ) {
		fault.pte_flags = VM_NONE;
		fault.reason = VM_READ;
		if ((err = do_page_fault(&fault)) < 0)
			return err;
	}
	/* Now write */
	if (vmflags & VM_WRITE) {
		fault.pte_flags = VM_READ;
		fault.reason = VM_WRITE;
		if ((err = do_page_fault(&fault)) < 0)
			return err;
	}
	/* No exec or any other fault reason allowed. */
	BUG_ON(vmflags & ~(VM_READ | VM_WRITE));

	return 0;
}


