/*
 * phase3d.c
 *
 */
// calculate the size of each page and use that to find the offset depending on which page that is
//usloss mmu page size

//# slots = # of sectors per page & disk size & num sectors per track

/***************

NOTES ON SYNCHRONIZATION

There are various shared resources that require proper synchronization. 

Swap space. Free swap space is a shared resource, we don't want multiple pagers choosing the
same free space to hold a page. You'll need a mutex around the free swap space.

The clock hand is also a shared resource.

The frames are a shared resource in that we don't want multiple pagers to choose the same frame via
the clock algorithm. That's the purpose of marking a frame as "busy" in the pseudo-code below. 
Pagers ignore busy frames when running the clock algorithm.

A process's page table is a shared resource with the pager. The process changes its page table
when it quits, and a pager changes the page table when it selects one of the process's pages
in the clock algorithm. 

Normally the pagers would perform I/O concurrently, which means they would release the mutex
while performing disk I/O. I made it simpler by having the pagers hold the mutex while they perform
disk I/O.

***************/


#include <assert.h>
#include <phase1.h>
#include <phase2.h>
#include <usloss.h>
#include <string.h>
#include <libuser.h>

#include "phase3.h"
#include "phase3Int.h"

#ifdef DEBUG
static int debugging3 = 1;
#else
static int debugging3 = 0;
#endif


//data structure represents the memory in the swap disk
//page, frame, pid, in use
struct slot {
    int page;
    int frame;
    int pid;
    int in_use;
}typedef slot;

struct frameInfo {
    int busy;
} typedef frameInfo;

SID mutex;
slot* diskSlots;
frameInfo* frames;
static int init = FALSE;
static void debug3(char *fmt, ...)
{
    va_list ap;

    if (debugging3) {
        va_start(ap, fmt);
        USLOSS_VConsole(fmt, ap);
    }
}

int bytesInSector, sectorsInTrack, tracksInDisk, numSlots, pageSize, sectorsInPage;
USLOSS_PTE* pageTable;

/*
 *----------------------------------------------------------------------
 *
 * P3SwapInit --
 *
 *  Initializes the swap data structures.
 *
 * Results:
 *   P3_ALREADY_INITIALIZED:    this function has already been called
 *   P1_SUCCESS:                success
 *
 *----------------------------------------------------------------------
 */
int
P3SwapInit(int pages, int framesNum)
{
    if (init == TRUE) { return P3_ALREADY_INITIALIZED; }
    int result = P1_SUCCESS;
    int i, rc;

    rc = P2_DiskSize(P3_SWAP_DISK, &bytesInSector, &sectorsInTrack, &tracksInDisk); assert(rc == P1_SUCCESS);

    pageSize = USLOSS_MmuPageSize();

    sectorsInPage = pageSize / bytesInSector;

    numSlots = (sectorsInTrack / sectorsInPage) * tracksInDisk;

    USLOSS_Console("%d\n", numSlots);

    frames = malloc(sizeof(frameInfo) * framesNum);
    for (i = 0; i < framesNum; i++) { frames[i].busy = FALSE; }

    diskSlots = malloc(sizeof(slot) * numSlots);
    for (i = 0; i < numSlots; i++){
        diskSlots[i].in_use = FALSE;
        diskSlots[i].pid = -1;
        diskSlots[i].page = -1;
        diskSlots[i].frame = -1;
    }


    rc = P1_SemCreate("Mutex", 1, &mutex); assert (rc == P1_SUCCESS);

    pageTable = malloc(sizeof(USLOSS_PTE) * P1_MAXPROC);

    P3_vmStats.pages = pages;
    P3_vmStats.frames = framesNum;
    P3_vmStats.freeFrames = framesNum;
    P3_vmStats.blocks = numSlots;
    P3_vmStats.freeBlocks = numSlots;

    init = TRUE;
    return result;
}
/*
 *----------------------------------------------------------------------
 *
 * P3SwapShutdown --
 *
 *  Cleans up the swap data structures.
 *
 * Results:
 *   P3_NOT_INITIALIZED:    P3SwapInit has not been called
 *   P1_SUCCESS:            success
 *
 *----------------------------------------------------------------------
 */
int
P3SwapShutdown(void)
{
    int rc, result = P1_SUCCESS;
    if (init == FALSE) { return P3_NOT_INITIALIZED; }
    // clean things up
    free(frames);
    free(diskSlots);
    free(pageTable);
    rc = P1_SemFree(mutex); assert (rc == P1_SUCCESS);

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * P3SwapFreeAll --
 *
 *  Frees all swap space used by a process
 *
 * Results:
 *   P3_NOT_INITIALIZED:    P3SwapInit has not been called
 *   P1_SUCCESS:            success
 *
 *----------------------------------------------------------------------
 */

int
P3SwapFreeAll(int pid)
{
    int result = P1_SUCCESS;
    int rc;

    /*****************

    P(mutex)
    free all swap space used by the process
    V(mutex)

    *****************/


    if(init == FALSE){ return P3_NOT_INITIALIZED; }

    rc = P1_P(mutex); assert(rc == P1_SUCCESS);

    USLOSS_PTE* pte;
    rc = P3PageTableGet(pid, &pte); assert(rc == P1_SUCCESS);

    for (int j = 0; j < P3_vmStats.pages; j++) {
        USLOSS_Console("Page: %d, Frame: %d, Incore: %d\n", j, pte[j].frame, pte[j].incore);

    }


    for(int i = 0; i < numSlots; i++){
        USLOSS_Console("Slot: %d, in_use: %d, Page: %d, PID: %d, Frame: %d\n", i, diskSlots[i].in_use, diskSlots[i].page, diskSlots[i].pid, diskSlots[i].frame);
        if(diskSlots[i].pid == pid){
            diskSlots[i].in_use = FALSE;
            diskSlots[i].page = -1;
            diskSlots[i].pid = -1;
            diskSlots[i].frame = -1;

            P3_vmStats.freeBlocks++;
        }
    }
    rc = P1_V(mutex); assert(rc == P1_SUCCESS);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * P3SwapOut --
 *
 * Uses the clock algorithm to select a frame to replace, writing the page that is in the frame out 
 * to swap if it is dirty. The page table of the page’s process is modified so that the page no 
 * longer maps to the frame. The frame that was selected is returned in *frame. 
 *
 * Results:
 *   P3_NOT_INITIALIZED:    P3SwapInit has not been called
 *   P1_SUCCESS:            success
 *
 *----------------------------------------------------------------------
 */
int
P3SwapOut(int *frame) 
{
    int result = P1_SUCCESS;
    int target, rc, access, index;

    static int hand = -1;
    rc = P1_P(mutex); assert (rc == P1_SUCCESS);
    while(TRUE) {
        hand = (hand + 1) % P3_vmStats.frames;
        if (frames[hand].busy == FALSE) {
            rc = USLOSS_MmuGetAccess(hand, &access); assert (rc == USLOSS_MMU_OK);
            if ((access & USLOSS_MMU_REF) != USLOSS_MMU_REF) {
                target = hand;
                break;
            } else {
                rc = USLOSS_MmuSetAccess(hand, (access & (~USLOSS_MMU_REF))); assert (rc == USLOSS_MMU_OK);
            }
        }
    }

//    rc = USLOSS_MmuGetAccess(target, &access); assert (rc == USLOSS_MMU_OK);
    if ((access & USLOSS_MMU_DIRTY) == USLOSS_MMU_DIRTY) {
//        write page to its location on the swap disk (P3FrameMap,P2_DiskWrite,P3FrameUnmap)
        void* address;
        rc = P3FrameMap(target, &address); assert (rc == P1_SUCCESS);

        for (index = 0; index < numSlots; index++) {
            if (diskSlots[index].frame == target) { break; }
        }

        if (index == numSlots) { USLOSS_Console("big problemo\n"); }

        int currentTrack = ((index * pageSize) / bytesInSector) / sectorsInTrack;
        int currentSector = ((index * pageSize) / bytesInSector) % sectorsInTrack;

        void* pageBuffer = malloc(pageSize);

        memcpy(pageBuffer, address, pageSize);

        rc = P2_DiskWrite(P3_SWAP_DISK, currentTrack, currentSector, sectorsInPage, pageBuffer); assert(rc == P1_SUCCESS);
        // printf("process %d writing into slot %d from frame %d\n", diskSlots[index].pid, index, target);
        printf("Writing %s \n to sector %d in track %d in slot %d\n\n", (char *)pageBuffer, currentSector, currentTrack, index);
        free(pageBuffer);

        rc = P3FrameUnmap(target); assert (rc == P1_SUCCESS);

        rc = USLOSS_MmuSetAccess(target, (access & (~USLOSS_MMU_DIRTY))); assert (rc == USLOSS_MMU_OK);
    }


    USLOSS_PTE* pte;

    for (index = 0; index < numSlots; index++) {
        if (diskSlots[index].frame == target && diskSlots[index].in_use == TRUE) {
            int pid = diskSlots[index].pid;
            rc = P3PageTableGet(pid, &pte); assert(rc == P1_SUCCESS);

            for (int index2 = 0; index2 < P3_vmStats.pages; index2++) {
                if (pte[index2].frame == target) {
                    pte[index2].incore = FALSE;

                    break;
                }
            }
            break;
        }
    }

    frames[target].busy = TRUE;
    *frame = target;
    rc = P1_V(mutex); assert (rc == P1_SUCCESS);


    /*****************

    NOTE: in the pseudo-code below I used the notation frames[x] to indicate frame x. You 
    may or may not have an actual array with this name. As with all my pseudo-code feel free
    to ignore it.


    static int hand = -1;    // start with frame 0
    P(mutex)
    loop
        hand = (hand + 1) % # of frames
        if frames[hand] is not busy
            if frames[hand] hasn't been referenced (USLOSS_MmuGetAccess)
                target = hand
                break
            else
                clear reference bit (USLOSS_MmuSetAccess)
    if frame[target] is dirty (USLOSS_MmuGetAccess)
        write page to its location on the swap disk (P3FrameMap,P2_DiskWrite,P3FrameUnmap)
        clear dirty bit (USLOSS_MmuSetAccess)
    update page table of process to indicate page is no longer in a frame
    mark frames[target] as busy
    V(mutex)
    *frame = target

    *****************/

    return result;
}
/*
 *----------------------------------------------------------------------
 *
 * P3SwapIn --
 *
 *  Opposite of P3FrameMap. The frame is unmapped.
 *
 * Results:
 *   P3_NOT_INITIALIZED:     P3SwapInit has not been called
 *   P1_INVALID_PID:         pid is invalid      
 *   P1_INVALID_PAGE:        page is invalid         
 *   P1_INVALID_FRAME:       frame is invalid
 *   P3_EMPTY_PAGE:          page is not in swap
 *   P1_OUT_OF_SWAP:         there is no more swap space
 *   P1_SUCCESS:             success
 *
 *----------------------------------------------------------------------
 */
int
P3SwapIn(int pid, int page, int frame)
{
    int result = P1_SUCCESS;
    int rc, index;
    int found = FALSE;
    void* address;

    USLOSS_Console("PID: %d, page: %d, frame: %d\n", pid, page, frame);


    //set slot frame  = frame
    rc = P1_P(mutex); assert (rc == P1_SUCCESS);
//    if page is on swap disk
    for (index = 0; index < numSlots; index++) {
        if (diskSlots[index].pid == pid && diskSlots[index].page == page && diskSlots[index].in_use == TRUE) {
            found = TRUE;
            break;
        }
    }

    if (found) {
        // read page from swap disk into frame (P3FrameMap,P2_DiskRead,P3FrameUnmap)
        rc = P3FrameMap(frame, &address); assert(rc == P1_SUCCESS);
        printf("her2\n");
        int currentTrack = ((index * pageSize) / bytesInSector) / sectorsInTrack;
        int currentSector = ((index * pageSize) / bytesInSector) % sectorsInTrack;

        void* pageBuffer = malloc(pageSize);

        rc = P2_DiskRead(P3_SWAP_DISK, currentTrack, currentSector, sectorsInPage, pageBuffer); assert(rc == P1_SUCCESS);

        memcpy(address, pageBuffer, pageSize);
       // printf("process %d reading from slot %d into frame %d\n", diskSlots[index].pid, index, frame);
        printf("Reading %s\n from sector %d in track %d in slot %d\n\n", (char *)pageBuffer, currentSector, currentTrack, index);
        free(pageBuffer);
        diskSlots[index].frame = frame;
        rc = P3FrameUnmap(frame); assert (rc == P1_SUCCESS);
//            for(int i = 0; i < numSlots; i++){
//                if(diskSlots[i].pid == pid && diskSlots[i].frame == frame && diskSlots[i].page != page){
//                    diskSlots[i].frame = -1;
//                }
//            }
    } else {
        int slotFound = FALSE;

        for (index = 0; index < numSlots; index++) {
            if (diskSlots[index].in_use == FALSE) {
                printf("here");
                slotFound = TRUE;

                diskSlots[index].in_use = TRUE;
                diskSlots[index].page = page;
                diskSlots[index].pid = pid;
                diskSlots[index].frame = frame;

                P3_vmStats.freeBlocks--;

                break;
            }
        }

        result = slotFound? P3_EMPTY_PAGE: P3_OUT_OF_SWAP;
    }

    frames[frame].busy = FALSE;
    rc = P1_V(mutex); assert (rc == P1_SUCCESS);

    /*****************

    P(mutex)
    if page is on swap disk
        read page from swap disk into frame (P3FrameMap,P2_DiskRead,P3FrameUnmap)
    else
        allocate space for the page on the swap disk
        if no more space
            result = P3_OUT_OF_SWAP
        else
            result = P3_EMPTY_PAGE
    mark frame as not busy
    V(mutex)

    *****************/

    return result;
}