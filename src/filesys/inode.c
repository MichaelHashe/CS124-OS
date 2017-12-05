#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/cache.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include <bitmap.h>

#include <stdio.h>

/*! Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/*! On-disk inode.
    Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
    block_sector_t start; // TODO: delete
    off_t length;                       /*!< File size in bytes. */

    // TODO: add multilevel indirection details
    block_sector_t double_indirect;

    unsigned magic;                     /*!< Magic number. */
    // TODO: we should actually make use of this number by asserting it in 
    // different places
    uint32_t unused[124];               /*!< Not used. */
};

/*! Returns the number of sectors to allocate for an inode SIZE
    bytes long. */
static inline size_t bytes_to_sectors(off_t size) {
    return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE);
}

/*! In-memory inode. */
struct inode {
    struct list_elem elem;              /*!< Element in inode list. */
    block_sector_t sector;              /*!< Sector number of disk location. */
    int open_cnt;                       /*!< Number of openers. */
    bool removed;                       /*!< True if deleted, false otherwise. */
    int deny_write_cnt;                 /*!< 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /*!< Inode content. */
    // TODO: remove this data attribute and set it to be a pointer to data that is read by the cache (and casted into inode_disk *) from the sector of inode_disk given by inode.sector
};


static void indices_from_offset(off_t pos, size_t *dir_idx, size_t *ind_idx);
static bool inode_alloc_for_append(size_t cnt, struct inode_disk *data);


/*! Returns the block device sector that contains byte offset POS
    within INODE.
    Returns -1 if INODE does not contain data for a byte at offset
    POS. */
static block_sector_t byte_to_sector(const struct inode *inode, off_t pos) {
    // TODO: change this to use indexing instead of assuming continguous data sectors
    ASSERT(inode != NULL);
    if (pos < inode->data.length)
        return inode->data.start + pos / BLOCK_SECTOR_SIZE;
    else
        return -1;
}

static void indices_from_offset(off_t pos, size_t *dir_idx, size_t *ind_idx) {
    *dir_idx = pos / BLOCK_SECTOR_SIZE;
    *ind_idx = *dir_idx / NUM_ENTRIES_IN_INDIRECT;
    *dir_idx %= NUM_ENTRIES_IN_INDIRECT;
}


static bool inode_alloc_for_append(size_t cnt, struct inode_disk *data) {
    // Get correct cnt of inodes we need
    size_t i;
    size_t dir_idx_f, ind_idx_f, dir_idx, ind_idx;
    indices_from_offset(data->length + cnt, &dir_idx_f, &ind_idx_f);
    indices_from_offset(data->length, &dir_idx, &ind_idx);

    size_t file_sectors = (ind_idx_f - ind_idx) * NUM_ENTRIES_IN_INDIRECT + dir_idx;
    size_t new_sectors = file_sectors + (ind_idx_f - ind_idx);

    /* If that are not available free sectors for allocation, return false. */
    size_t num_sectors = bitmap_size(free_map);
    if (bitmap_count(free_map, 0, num_sectors, false) < new_sectors) {
        return false;
    }
    
    /* Else, we allocate the sectors and fill them into the inode_disk data. */
    size_t num_found = 0;
    
    block_sector_t *dir = malloc(BLOCK_SECTOR_SIZE);
    if (dir == NULL) {
        return false;
    }
    block_sector_t *ind = malloc(BLOCK_SECTOR_SIZE);
    if (ind == NULL) {
        free(dir);
        return false;
    }
    cache_read(data->double_indirect, ind);
    cache_read(ind[ind_idx], dir);

    // TODO: perhaps don't statically allocate all sectors initially?
    block_sector_t available_sectors[new_sectors];

    for (i = 0; i < num_sectors; i++) {
         /* We found a sector that is free, so give it to the inode. */
        if (!bitmap_test(free_map, i)) {
            bitmap_mark(free_map, i);

            available_sectors[num_found] = i;
            num_found++;
            if (num_found == new_sectors) {
                break;
            }
        }
    }

    /* Make sure that we were able to allocate all blocks that we needed to. */
    ASSERT(num_found == new_sectors);

    /* Write all new entries into the indirected sectors. */
    i = 0;
    while (i < new_sectors) {
        dir_idx++;

        if (dir_idx == NUM_ENTRIES_IN_INDIRECT) {
            cache_write(ind[ind_idx], dir);
            ind_idx++;
            dir_idx = 0;
            ind[ind_idx] = available_sectors[i];
            i++;
            memset(ind, 0, BLOCK_SECTOR_SIZE);
        }

        dir[dir_idx] = available_sectors[i];

        i++;
    }

    /* Finish persisting to disk our changes and free temporary buffers. */
    cache_write(ind[ind_idx], dir);
    cache_write(data->double_indirect, ind);
    free(dir);
    free(ind);

    /* Set the new file length accordingly. */
    data->length += cnt;

    return true;
}


/*! List of open inodes, so that opening a single inode twice
    returns the same `struct inode'. */
static struct list open_inodes;

/*! Initializes the inode module. */
void inode_init(void) {
    list_init(&open_inodes);
}

/*! Initializes an inode with LENGTH bytes of data and
    writes the new inode to sector SECTOR on the file system
    device.
    Returns true if successful.
    Returns false if memory or disk allocation fails. */
bool inode_create(block_sector_t sector, off_t length) {
    struct inode_disk *disk_inode = NULL;
    bool success = false;

    ASSERT(length >= 0);

    /* If this assertion fails, the inode structure is not exactly
       one sector in size, and you should fix that. */
    ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

    disk_inode = calloc(1, sizeof *disk_inode);
    if (disk_inode != NULL) {
        size_t sectors = bytes_to_sectors(length);
        disk_inode->length = length;
        disk_inode->magic = INODE_MAGIC;

        /* Initialize multilevel indirection details. */

        if (free_map_allocate(sectors, &disk_inode->start)) {
            cache_write(sector, disk_inode, BLOCK_SECTOR_SIZE, 0);
            if (sectors > 0) {
                static char zeros[BLOCK_SECTOR_SIZE];
                size_t i;
              
                for (i = 0; i < sectors; i++) 
                    cache_write(disk_inode->start + i, zeros, BLOCK_SECTOR_SIZE, 0);
            }
            success = true; 
        }
        free(disk_inode);
    }


    return success;
}

/*! Reads an inode from SECTOR
    and returns a `struct inode' that contains it.
    Returns a null pointer if memory allocation fails. */
struct inode * inode_open(block_sector_t sector) {
    struct list_elem *e; 
    struct inode *inode;

    /* Check whether this inode is already open. */
    for (e = list_begin(&open_inodes); e != list_end(&open_inodes);
         e = list_next(e)) {
        inode = list_entry(e, struct inode, elem);
        if (inode->sector == sector) {
            inode_reopen(inode);
            return inode; 
        }
    }

    /* Allocate memory. */
    inode = malloc(sizeof *inode);
    if (inode == NULL)
        return NULL;

    /* Initialize. */
    list_push_front(&open_inodes, &inode->elem);
    inode->sector = sector;
    inode->open_cnt = 1;
    inode->deny_write_cnt = 0;
    inode->removed = false;
    cache_read(inode->sector, &inode->data, BLOCK_SECTOR_SIZE, 0);
    return inode;
}

/*! Reopens and returns INODE. */
struct inode * inode_reopen(struct inode *inode) {
    if (inode != NULL)
        inode->open_cnt++;
    return inode;
}

/*! Returns INODE's inode number. */
block_sector_t inode_get_inumber(const struct inode *inode) {
    return inode->sector;
}

/*! Closes INODE and writes it to disk.
    If this was the last reference to INODE, frees its memory.
    If INODE was also a removed inode, frees its blocks. */
void inode_close(struct inode *inode) {
    /* Ignore null pointer. */
    if (inode == NULL)
        return;

    /* Release resources if this was the last opener. */
    if (--inode->open_cnt == 0) {
        /* Remove from inode list and release lock. */
        list_remove(&inode->elem);
 
        /* Deallocate blocks if removed. */
        if (inode->removed) {
            free_map_release(inode->sector, 1);
            free_map_release(inode->data.start,
                             bytes_to_sectors(inode->data.length)); 
        }

        free(inode); 
    }
}

/*! Marks INODE to be deleted when it is closed by the last caller who
    has it open. */
void inode_remove(struct inode *inode) {
    ASSERT(inode != NULL);
    inode->removed = true;
}

/*! Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at(struct inode *inode, void *buffer_, off_t size, off_t offset) {
    uint8_t *buffer = buffer_;
    off_t bytes_read = 0;

    while (size > 0) {
        /* Disk sector to read, starting byte offset within sector. */
        block_sector_t sector_idx = byte_to_sector (inode, offset);
        int sector_ofs = offset % BLOCK_SECTOR_SIZE;

        /* Bytes left in inode, bytes left in sector, lesser of the two. */
        off_t inode_left = inode_length(inode) - offset;
        int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
        int min_left = inode_left < sector_left ? inode_left : sector_left;

        /* Number of bytes to actually copy out of this sector. */
        int chunk_size = size < min_left ? size : min_left;
        if (chunk_size <= 0)
            break;

        cache_read(sector_idx, buffer + bytes_read, chunk_size, sector_ofs);
      
        /* Advance. */
        size -= chunk_size;
        offset += chunk_size;
        bytes_read += chunk_size;
    }

    return bytes_read;
}

/*! Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
    Returns the number of bytes actually written, which may be
    less than SIZE if end of file is reached or an error occurs.
    (Normally a write at end of file would extend the inode, but
    growth is not yet implemented.) */
off_t inode_write_at(struct inode *inode, const void *buffer_, off_t size, off_t offset) {
    const uint8_t *buffer = buffer_;
    off_t bytes_written = 0;
    // uint8_t *bounce = NULL;

    if (inode->deny_write_cnt)
        return 0;

    while (size > 0) {
        /* Sector to write, starting byte offset within sector. */
        block_sector_t sector_idx = byte_to_sector(inode, offset);
        int sector_ofs = offset % BLOCK_SECTOR_SIZE;

        /* Bytes left in inode, bytes left in sector, lesser of the two. */
        off_t inode_left = inode_length(inode) - offset;
        int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
        int min_left = inode_left < sector_left ? inode_left : sector_left;

        /* Number of bytes to actually write into this sector. */
        int chunk_size = size < min_left ? size : min_left;
        if (chunk_size <= 0)
            break;


        cache_write(sector_idx, buffer + bytes_written, chunk_size, sector_ofs);

        /* Advance. */
        size -= chunk_size;
        offset += chunk_size;
        bytes_written += chunk_size;
    }

    return bytes_written;
}

/*! Disables writes to INODE.
    May be called at most once per inode opener. */
void inode_deny_write (struct inode *inode) {
    inode->deny_write_cnt++;
    ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/*! Re-enables writes to INODE.
    Must be called once by each inode opener who has called
    inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write (struct inode *inode) {
    ASSERT(inode->deny_write_cnt > 0);
    ASSERT(inode->deny_write_cnt <= inode->open_cnt);
    inode->deny_write_cnt--;
}

/*! Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode *inode) {
    return inode->data.length;
}

