#!/usr/bin/env python3
"""Self-heal the WM surface teardown race fix (syscall.c).

The bug: wm_surface_teardown() unmapped the WM-side surface VA the moment an
app died, but the compositor could still be holding that VA between two scan
passes -> SIGSEGV ("task 4 'wm' killed").  The fix keeps the WM-side mapping
+ frames alive (deferred free) and reclaims them when the slot is reused.
Run by ensure_kernel.py; idempotent."""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
p = os.path.join(ROOT, "kernel", "arch", "x86_64", "syscall.c")
s = open(p).read()

MARKER = "WM side: KEEP the mapping + frames ALIVE (deferred free)"
if MARKER in s:
    print("[ok] wm surface teardown deferred-free")
else:
    old_t = r'''    if (s->wm_mapped && g_wm_task && g_wm_task->pml4) {
        for (u32 i = 0; i < s->npages; i++)
            vmm_unmap_in(g_wm_task->pml4, s->wm_va + i * PAGE_SIZE);
        s->wm_mapped = false;
        /* shoot down the wm's stale TLB entries before the frames below
         * are freed - otherwise the wm's CPU could keep painting from a
         * freed surface (use-after-free on the screen). */
        if (g_wm_task != cur())
            smp_tlb_shootdown_all();
    }
    /* table refs: free the frames for real */
    for (u32 i = 0; i < s->npages; i++)
        pmm_free_page(s->pages[i]);
    if (g_focus_pid == s->owner_pid) g_focus_pid = 0;
    kprintf("wm: surface %u destroyed (owner pid %u)\n", s->id, s->owner_pid);
    memset(s, 0, sizeof *s);
}'''
    new_t = r'''    /* WM side: KEEP the mapping + frames ALIVE (deferred free).
     * The owner may die between two of the compositor's scan passes; if we
     * unmapped the wm-side VA here, the wm would still be holding the stale
     * VA and paint from it -> SIGSEGV (the classic "wm killed" crash).
     * Keeping the pages mapped means the wm can only ever read valid (stale)
     * content until its next scan removes the window.  The frames are freed
     * when the slot is reused by sys_wm_create. */
    if (g_focus_pid == s->owner_pid) g_focus_pid = 0;
    kprintf("wm: surface %u destroyed (owner pid %u)\n", s->id, s->owner_pid);
    {
        u32 saved_n = s->npages;
        u32 saved[WM_SURF_MAX_PAGES];
        for (u32 i = 0; i < saved_n; i++) saved[i] = s->pages[i];
        memset(s, 0, sizeof *s);
        s->npages = saved_n;
        for (u32 i = 0; i < saved_n; i++) s->pages[i] = saved[i];
        s->used = false;
        s->wm_mapped = true;   /* pages + wm-side refs still held */
    }
}'''
    old_c = r'''    u32 npages = (w * h * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
    if (npages > WM_SURF_MAX_PAGES) return -1;

    memset(s, 0, sizeof *s);
    s->used = true;'''
    new_c = r'''    u32 npages = (w * h * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
    if (npages > WM_SURF_MAX_PAGES) return -1;

    /* Deferred cleanup: a previous surface in this slot kept its wm mapping
     * + frames after teardown (see wm_surface_teardown).  Reclaim them now.
     * The stale wm-side PTEs are overwritten below by the new mapping. */
    u32 old_n = s->npages;
    paddr_t old_pages[WM_SURF_MAX_PAGES];
    for (u32 i = 0; i < old_n && i < WM_SURF_MAX_PAGES; i++)
        old_pages[i] = s->pages[i];

    memset(s, 0, sizeof *s);
    s->used = true;'''
    old_r = r'''    sched_charge_pages((i64)npages);'''
    new_r = r'''    /* Reclaim the deferred frames from the previous surface in this slot
     * (their stale wm-side PTEs were just overwritten above). */
    for (u32 i = 0; i < old_n && i < WM_SURF_MAX_PAGES; i++) {
        pmm_unref_page(old_pages[i]);   /* the old wm-side mapping's ref */
        pmm_free_page(old_pages[i]);    /* the old table's own ref        */
    }
    sched_charge_pages((i64)npages);'''
    done = []
    if old_t in s:
        s = s.replace(old_t, new_t, 1); done.append("teardown")
    if old_c in s:
        s = s.replace(old_c, new_c, 1); done.append("create")
    if old_r in s and "Reclaim the deferred frames" not in s:
        s = s.replace(old_r, new_r, 1); done.append("reclaim")
    if done:
        open(p, "w").write(s)
        print("[+] wm surface deferred-free fix:", ", ".join(done))
    else:
        print("[??] wm surface teardown/create anchors not found")
