#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "devices/block.h"
#include "threads/thread.h"
#include "threads/malloc.h"

/*! Partition that contains the file system. */
struct block *fs_device;

static void do_format(void);
static bool split_path_parent_name(const char *path, char ** parent_dir_name, 
                    char ** name, bool is_directory);

/*! Initializes the file system module.
    If FORMAT is true, reformats the file system. */
void filesys_init(bool format) {
    fs_device = block_get_role(BLOCK_FILESYS);
    if (fs_device == NULL)
        PANIC("No file system device found, can't initialize file system.");

    inode_init();
    free_map_init();

    if (format) 
        do_format();

    free_map_open();
}

/*! Shuts down the file system module, writing any unwritten data to disk. */
void filesys_done(void) {
    flush_cache();
    free_map_close();
}

static bool split_path_parent_name(const char *path, char ** parent_dir_name, 
                    char ** name, bool is_directory) {
    int end_of_name = strlen(path);
    int end_of_parent;

    if (is_directory) {
        while (*(path + end_of_name) == '/') {
            end_of_name--;
        }
    } else {
        if (*(path + end_of_name) == '/') {
            return false;
        }
    }

    if (end_of_name == 0) {
        return false;
    }

    end_of_parent = end_of_name;
    while (*(path + end_of_parent) != '/') {
        if (end_of_parent == 0) {
            break;
        }
        end_of_parent--;
    }

    if (end_of_parent == 0) {
        if (*path == '/') {
            end_of_parent++;
        }
        *name = (char *) path + end_of_parent;
    } else {
        *name = ((char *) path) + end_of_parent + 1;
    }

    *parent_dir_name = malloc(end_of_parent + 1);
    if (*parent_dir_name == NULL) {
        return false;
    }
    strlcpy(*parent_dir_name, path, end_of_parent + 1);

    return true;
}

/*! Creates a file named NAME with the given INITIAL_SIZE.  Returns true if
    successful, false otherwise.  Fails if a file named NAME already exists,
    or if internal memory allocation fails. */
bool filesys_create(const char *path, off_t initial_size, bool is_directory) {
    block_sector_t inode_sector = 0;

    /* Get parent directory and name of file/directory to be created at  
    path. */
    char *parent_dir_name;
    char *name;
    if (!split_path_parent_name(path, &parent_dir_name, &name, is_directory)) {
        return false;
    }

    /* Get file corresponding to parent directory. */
    struct file *parent_dir_file = filesys_open((const char *) parent_dir_name);
    free(parent_dir_name);

    if (parent_dir_file == NULL) {
        return false;
    }

    struct inode *parent_inode = file_get_inode(parent_dir_file);
    block_sector_t parent_sector = inode_get_sector(parent_inode);

    /* If inode has been removed, can't create files. */
    if (inode_is_removed(parent_inode)) {
        return false;
    }

    /* Only allow one process to create a file at a time. */
    directory_lock(parent_inode);

    /* Get directory struct corresponding to parent directory. */
    struct dir * parent_dir = dir_open(parent_inode);

    /* If parent already has a child with the same name, then return false. */
    struct inode *inode = NULL;
    if (dir_lookup(parent_dir, name, &inode)){
        return false;
    }

    /* Create new file/directory inside its parent directory. */
    bool success = (parent_dir != NULL &&
                    free_map_allocate_single(&inode_sector) &&
                    inode_create(inode_sector, initial_size, is_directory) &&
                    dir_add(parent_dir, name, inode_sector));

    /* Add relative links to parent and self in new directory directory. */
    if (success && is_directory) {
        struct dir *new_dir = dir_open(inode_open(inode_sector));
        if (new_dir != NULL) {
            char relative_link[] =  "..";
            dir_add(new_dir, relative_link, parent_sector);
            relative_link[1] = '\0';
            dir_add(new_dir, relative_link, inode_sector); 
        } else {
            success = false;
        }
        dir_close(new_dir);
    }

    /* Mark parent directory as having a new file. */
    if (success) {
        inode_incr_count(parent_inode);
    }

    if (!success && inode_sector != 0) 
        free_map_release(inode_sector, 1);

    dir_close(parent_dir);
    directory_release(parent_inode);

    return success;
}

/*! Opens the file with the given PATH.  Returns the new file if successful
    or a null pointer otherwise.  Fails if no file named NAME exists,
    or if an internal memory allocation fails. */
struct file * filesys_open(const char *path) {
    struct inode *inode = NULL;
    struct dir *dir;

    /* If path is empty, just return the current directory. 
    Treat as an empty relative path. */
    if (path[0] == '\0') {
        return file_open(inode_open(thread_current()->cur_directory));
    }

    /* If name is an absolute path, start looking from root. Else, look from 
    the current open directory. */
    if (path[0] == '/') {
        dir = dir_open_root();
        path++;
    } else {
        dir = dir_open(inode_open(thread_current()->cur_directory));
    }

    if (dir != NULL){
        dir_lookup(dir, path, &inode);
    }
    dir_close(dir);
    return file_open(inode);
}

/*! Deletes the file named NAME.  Returns true if successful, false on failure.
    Fails if no file named NAME exists, or if an internal memory allocation
    fails. */
bool filesys_remove(const char *path) {
    /* Get parent directory and name of file/directory to be removed at  
    path. */
    char *parent_dir_name;
    char *name;
    if (!split_path_parent_name(path, &parent_dir_name, &name, true)) {
        return false;
    }

    struct file * file = filesys_open(parent_dir_name);
    if (file == NULL) {
        return false;
    }

    struct file *rem_file = filesys_open(path);
    if (rem_file == NULL) {
        return false;
    }

    /* Will need to access inodes for both parent and child. */
    struct inode* parent_inode = file_get_inode(file);
    
    /* But, only allow modification of inode if we have the directory lock. */
    directory_lock(parent_inode);
    
    struct inode *child_inode = file_get_inode(filesys_open(path));
    

    /* Can only delete file if empty. */
    if (inode_num_files(child_inode)) {
        return false;
    }

    struct dir *dir = dir_open(parent_inode);
    bool success = dir != NULL && dir_remove(dir, name);
    dir_close(dir);

    /* Parent directory has one fewer file. */
    inode_decr_count(parent_inode);

    /* Child directory is gone, and shouldn't have . or .. files. */
    struct dir *child_dir = dir_open(child_inode);
    dir_remove(child_dir, ".");
    dir_remove(child_dir, "..");
    dir_close(child_dir);
    inode_remove(child_inode);

    directory_release(parent_inode);
    return success;
}

/*! Formats the file system. */
static void do_format(void) {
    printf("Formatting file system...");
    free_map_create();

    if (!dir_create(ROOT_DIR_SECTOR, MAX_FILES_PER_DIR))
        PANIC("root directory creation failed");
    free_map_close();

    struct dir *new_dir = dir_open_root();
    if (new_dir == NULL) {
        PANIC("root directory creation failed");
    }

    /* Add relative links to parent and current directories as root. */
    char relative_link[] =  "..";
    dir_add(new_dir, relative_link, ROOT_DIR_SECTOR);
    relative_link[1] = '\0';
    dir_add(new_dir, relative_link, ROOT_DIR_SECTOR); 
}

