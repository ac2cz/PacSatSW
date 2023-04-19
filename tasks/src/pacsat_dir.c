/*
 * pacsat_dir.c
 *
 *  Created on: Mar 9, 2023
 *      Author: g0kla
 *
 *
 *
 * This file also includes a simple MRAM File System:
 * This is a very simple file storage system THAT IS NOT SUITABLE FOR FLIGHT
 * that uses the file_id as the file handle.
 *
 */
#include <assert.h>
#include "MRAMmap.h"
#include "nonvol.h"
#include "PbTask.h"
#include "pacsat_header.h"
#include "pacsat_dir.h"
#include "inet.h"
#include "redposix.h"
#include "str_util.h"

#ifdef DEBUG
#include "time.h"
#endif

/* Dir variables */
static DIR_NODE *dir_head = NULL;  // the head of the directory linked list
static DIR_NODE *dir_tail = NULL;  // the tail of the directory linked list
static uint32_t highest_file_id = 0; // This is incremented when we add files for upload.  Initialized when dir loaded.  TODO - keep only in MRAM

/* Forward declarations */
DIR_NODE * dir_add_pfh(char *file_path, HEADER *new_pfh);
void dir_delete_node(DIR_NODE *node);
void insert_after(DIR_NODE *p, DIR_NODE *new_node);

/* This is used by the Simple MRAM FS */
static HEADER pfh_buffer; // Static allocation of a header to use when we need to load/save the header details
static uint8_t pfh_byte_buffer[512]; /* Maximum size for a PFH TODO - what should size be.  STORE AS DEFINE */

/**
 * dir_next_file_number()
 *
 * This returns the next file number available for the upload process.
 * TODO - this will not cope well with failed uploads.  Those ids will be lost and
 * never used.  We are supposed to "reserve" the file number when a DATA command is
 * received, but we need to allocate it before that.
 *
 *
 * TODO ******** Clearly this needs to be saved in MRAM as the next file number.  We carry on from this number
 * even if all files are first deleted.  Otherwise we confuse the ground stations.
 *
 */
int dir_next_file_number() {
    highest_file_id++;
    return highest_file_id;
}

/**
 * dir_add_pfh()
 * Add a pacsat file header to the directory and return a pointer
 * to the inserted DIR_NODE in the linked list.
 *
 * This handles the situation where the list is empty and creates the first item.
 * In an existing list if this item does not have an upload time then it is new
 * and it is inserted at the end of the list with the current time.  If many items
 * are added at the same time then it is given an upload time 1 second after the
 * last item.
 * If this header already has an upload_time then we search backwards in the list
 * to find the insertion point.
 * If we find a header with the same upload_time then this must be a duplicate and it
 * is discarded.
 * To update an item correctly, remove it from the list, set the upload_time to zero
 * and then call this routine to insert it at the end.
 *
 * If the upload_time was modified then the pacsat file header is resaved to disk
 * * WARNING: This does not work if a header has fields that are not recognized by the sat.
   * Uploaded files must have their PFHs parsed and resaved with their data saved at the right
   * offset, before this process is run
 *
 * The linked list takes care of allocating and deallocating memory for the DIR_NODEs
 *
 * TODO Currently this memory is allocated on the heap with pvPortMalloc and pPortFree.  We may
 * want to change that to a completely static allocation and hold a free list for nodes that
 * are not used.  This could guarantee no memory fragmentation and the expense of complexity.
 *
 */
DIR_NODE * dir_add_pfh(char *file_name, HEADER *new_pfh) {
    int resave = false;
    DIR_NODE *new_node = (DIR_NODE *)pvPortMalloc(sizeof(DIR_NODE));
    new_node->file_id = new_pfh->fileId;
    strlcpy(new_node->filename, file_name, REDCONF_NAME_MAX+1U);
    new_node->body_offset = new_pfh->bodyOffset;
    new_node->upload_time = new_pfh->uploadTime;

    uint32_t now = getUnixTime(); // Get the time in seconds since the unix epoch
    if (new_node == NULL) return NULL; // ERROR
    if (dir_head == NULL) { // This is a new list
        dir_head = new_node;
        dir_tail = new_node;
        if (new_node->upload_time == 0) {
            new_node->upload_time = now;
            resave = true;
        }
        new_node->next = NULL;
        new_node->prev = NULL;
    } else if (new_node->upload_time == 0){
        /* Insert this at the end of the list as the newest item.  Make sure it has a unique upload time */
        if (dir_tail->upload_time >= now) {
            /* We have added more than one file within 1 second.  Add this at the next available second. */
            new_node->upload_time = dir_tail->upload_time+1;
        } else {
            new_node->upload_time = now;
        }
        insert_after(dir_tail, new_node);
        resave = true;
    } else {
        /* Insert this at the right point, searching from the back*/
        DIR_NODE *p = dir_tail;
        while (p != NULL) {
            if (p->upload_time == new_node->upload_time) {
                debug_print("ERROR: Attempt to insert duplicate PFH: ");
                //pfh_debug_print(mram_file);
                return NULL; // this is a duplicate
            } else if (p->upload_time < new_node->upload_time) {
                insert_after(p, new_node);
                break;
            } else if (p == dir_head) {
                // Insert at the head of the list
                new_node->next = p;
                p->prev = new_node;
                new_node->prev = NULL;
                dir_head = new_node;
                break;
            }
            p = p->prev;
        }
    }
    // Now re-save the file with the new time if it changed, this recalculates the checksums
    if (resave) {
        /* The length of the header and the file length do not change because we only change the upload time
         * So we can just re-write the header on top of the old with the new
         * upload time.  This will recalc the bytes and the checksums
         * WARNING: This does not work if a header has fields that are not recognized by the sat.
         * Uploaded files must have their PFHs parsed and resaved before this process is run
         */


        /* modify the uptime in the header */
        new_pfh->uploadTime = new_node->upload_time;

        /* Regenerate the bytes and generate the checksums.  FileSize is body_offset + body_size */
        uint16_t body_offset = pfh_generate_header_bytes(new_pfh, new_pfh->fileSize - new_pfh->bodyOffset, pfh_byte_buffer);
        if (body_offset != new_node->body_offset) {
            debug_print("ERROR: Regenerated Header size is incorrect.  Could not update header.\n");
            dir_delete_node(new_node);
            return FALSE;
        }
        char file_name_with_path[REDCONF_NAME_MAX+3U];
        snprintf(file_name_with_path, REDCONF_NAME_MAX+3U, "//%s",file_name);
        int32_t num = dir_fs_write_file_chunk(file_name_with_path, pfh_byte_buffer, body_offset, 0);
        if (num == -1) {
            // we could not save this
            debug_print("** Could not update the header for fh: %d to dir\n",new_node->file_id);
            dir_delete_node(new_node);
            return NULL;
        } else {
            debug_print("Saved: %d\n",new_node->file_id);
            //pfh_debug_print(new_pfh);
        }
    }
    return new_node;
}

/**
 * insert_after()
 * Insert new_node after node p in the linked list.
 * Handle the situation where p is the tail of the list
 * p may not be NULL
 *
 */
void insert_after(DIR_NODE *p, DIR_NODE *new_node) {
    assert(p != NULL);
    new_node->next = p->next; // which may be null if at end of list
    new_node->prev = p;
    if (p->next == NULL) // we are at the end of the list
        dir_tail = new_node;
    else
        p->next->prev = new_node;
    p->next = new_node;
}

/**
 * dir_delete_node()
 *
 * Remove an entry from the dir linked list and free the memory held by the node
 * and the pacsat file header.
 *
 * The files on disk are not removed.
 *
 */
void dir_delete_node(DIR_NODE *node) {
    if (node == NULL) return;
    if (node->prev == NULL && node->next == NULL) {
        // special case of only one item
        dir_head = NULL;
        dir_tail = NULL;
    } else if (node->prev == NULL) {
        // special case removing the head of the list
        dir_head = node->next;
        node->next->prev = NULL;
    } else if (node->next == NULL) {
        // special case removing the tail of the list
        dir_tail = node->prev;
        node->prev->next = NULL;

    } else {
        node->next->prev = node->prev;
        node->prev->next = node->next;
    }
//    debug_print("REMOVED: %d\n",node->mram_file->file_id);
//    pfh_debug_print(node->pfh);
    vPortFree(node);
}

/**
 * dir_free_list()
 *
 * Remove all entries from the dir linked list and free all the
 * memory held by the list and the pacsat file headers.
 */
void dir_free() {
    DIR_NODE *p = dir_head;
    while (p != NULL) {
        DIR_NODE *node = p;
        p = p->next;
        dir_delete_node(node);
    }
    debug_print("Dir List Cleared\n");
}

/**
 * dir_load_pacsat_file()
 *
 * Load a PACSAT file from MRAM and store it in the directory
 */
bool dir_load_pacsat_file(char *file_name) {
//    debug_print("Loading: %d from addr: %d \n", mram_file->file_id, mram_file->address);

//    int err = dir_validate_file(pfh,psf_name);
//    if (err != ER_NONE) {
//        error_print("Err: %d - validating: %s\n", err, psf_name);
//        return FALSE;
//    }
    //pfh_debug_print(pfh);
    char file_name_with_path[25];
    snprintf(file_name_with_path, 25, "//%s",file_name);

    // Read enough of the file to parse the PFH
    int32_t rc = dir_fs_read_file_chunk(file_name_with_path, pfh_byte_buffer, sizeof(pfh_byte_buffer), 0);
    if (rc == -1) {
        debug_print("Error reading file: %s\n",file_name_with_path);
        return FALSE;
    }
    uint16_t size;
    bool crc_passed = FALSE;
    pfh_extract_header(&pfh_buffer, pfh_byte_buffer, sizeof(pfh_byte_buffer), &size, &crc_passed);
    if (!crc_passed) { debug_print("CRC FAILED\n"); return FALSE;}

    DIR_NODE *p = dir_add_pfh(file_name, &pfh_buffer);
    if (p == NULL) {
        debug_print("** Could not add %s to dir\n", file_name_with_path);
        return FALSE;
    }
    if (pfh_buffer.fileId > highest_file_id)
        highest_file_id = pfh_buffer.fileId;
    return TRUE;
}

/**
 * dir_load()
 *
 * Load the directory from the MRAM and store it in uptime sorted order in
 * the linked list
 *
 */
int dir_load() {
    bool rc;
    REDDIR *pDir;
    char * path = "//";
    printf("Loading Directory from %s:\n",path);
    pDir = red_opendir(path);
    if (pDir == NULL) {
        printf("Unable to open dir: %s\n", red_strerror(red_errno));
        return FALSE;
    }

    REDDIRENT *pDirEnt;
    red_errno = 0; /* Set error to zero so we can distinguish between a real error and the end of the DIR */
    pDirEnt = red_readdir(pDir);
    while (pDirEnt != NULL) {
        debug_print("Loading: %s\n",pDirEnt->d_name);
        rc = dir_load_pacsat_file(pDirEnt->d_name);
        if (rc != TRUE) {
            debug_print("May need to remove potentially corrupt or duplicate PACSAT file: %d\n", pDirEnt->d_name);
            /* Don't automatically remove here, otherwise loading the dir twice actually deletes all the
             * files! */
        }
        pDirEnt = red_readdir(pDir);
    }
    if (red_errno != 0) {
        printf("Error reading directory: %s\n", red_strerror(red_errno));

    }
    int32_t rc2 = red_closedir(pDir);
    if (rc2 != 0) {
        printf("Unable to close file: %s\n", red_strerror(red_errno));
    }

    debug_print("DONE:\n");
    return TRUE;
}



/**
 * dir_get_pfh_by_date()
 *
 * Traverse the directory and return a pointer to the next DIR_NODE identified by the dates.
 * The node passed in is the previous node that we processed.  If there are no more nodes
 * then NULL is returned.
 *
 * The protocol defines the required functionality as:
 *
      For each PAIR, the server will transmit directories for those files with
      UPLOAD_TIME greater than or equal to <start> and less than or equal to <end>.

      If there are no files within the range, the directory for the first file with
      upload time greater than <end> will be transmitted. If there is no such file,
      the directory for this first file with upload time less than <start> will be
      transmitted. In either case, the <t_old> and <t_new> fields in this directory
      will indicate to the client that there are no entries between <start> and
      <end>.
 *
 * Returns a pointer to the DIR_NODE or NULL if none are found
 *
 */
DIR_NODE * dir_get_pfh_by_date(DIR_DATE_PAIR pair, DIR_NODE *p ) {
    if (p == NULL) {
        /* Then we are starting the search from the head.  TODO - could later optimize if search from head or tail */
        p = dir_head;
    }
    while (p != NULL) {
        DIR_NODE *node = p;
        p = p->next;
        if (node->upload_time >= pair.start && node->upload_time <= pair.end) {
            debug_print("-> returning file id: %04x name: %s\n",node->file_id, node->filename);
            return node;
        }
    }

    return NULL;
}

/**
 * dir_get_node_by_id()
 * Search for and return a file based on its id. If the file can not
 * be found then return NULL
 *
 */
DIR_NODE * dir_get_node_by_id(int file_id) {
    DIR_NODE *p = dir_head;
    while (p != NULL) {
        if (p->file_id == file_id)
            return p;
        p = p->next;
    }
    return NULL;
}

#ifdef DEBUG

/**
 * dir_debug_print()
 *
 * If DEBUG is set then print out all of the entries of the dir linked list.
 *
 */
void dir_debug_print(DIR_NODE *p) {
    if (p == NULL)
        p = dir_head;

    if (p == NULL)
        debug_print("..Empty Dir List\n");
    while (p != NULL) {
        //pfh_debug_print(p->pfh);
        char buf[30];
         time_t now = p->upload_time + 2208988800L;
         strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", gmtime(&now));
        debug_print("File id: %04x name: %s up:%d %s\n",p->file_id, p->filename, p->upload_time,buf);
        p = p->next;
    }
}

#endif /* DEBUG */

/**
 * FS FILE functions
 *
 */

/**
 * Write a file to the file system.  The file is first created or it is overwritten.
 * The full path to the file must be specified.
 *
 * Returns the number of bytes written or -1 if there is an error.
 *
 */
int32_t dir_fs_write_file_chunk(char *file_name_with_path, uint8_t *data, uint32_t length, uint32_t offset) {
    int32_t fp;
    int32_t numOfBytesWritten = -1;
    int32_t rc;

    fp = red_open(file_name_with_path, RED_O_CREAT | RED_O_WRONLY);
    if (fp == -1) {
        debug_print("Unable to open %s for writing: %s\n", file_name_with_path, red_strerror(red_errno));
        return -1;
    }

    if (offset != 0) {
        rc = red_lseek(fp, offset, RED_SEEK_SET);
        if (rc == -1) {
            debug_print("Unable to seek %s  to offset %d: %s\n", file_name_with_path, offset, red_strerror(red_errno));

            rc = red_close(fp);
            if (rc != 0) {
                printf("Unable to close %s: %s\n", file_name_with_path, red_strerror(red_errno));
            }
            return -1;
        }
    }

    numOfBytesWritten = red_write(fp, data, length);
    if (numOfBytesWritten != length) {
        printf("Write returned: %d\n",numOfBytesWritten);
        if (numOfBytesWritten == -1) {
            printf("Unable to write to %s: %s\n", file_name_with_path, red_strerror(red_errno));
        }
    }
    rc = red_close(fp);
    if (rc != 0) {
        printf("Unable to close %s: %s\n", file_name_with_path, red_strerror(red_errno));
    }

    return numOfBytesWritten;
}

/**
 * Read a chunk of bytes from a file in MRAM File system.
 * The full path must be specified for the file.
 *
 * Returns the number of bytes read or -1 if there was an error.
 *
 */
int32_t dir_fs_read_file_chunk(char *file_name_with_path, uint8_t *read_buffer, uint32_t length, uint32_t offset) {
    int32_t rc;

    int32_t fp = red_open(file_name_with_path, RED_O_RDONLY);
    if (fp == -1) {
        debug_print("Unable to open %s for reading: %s\n", file_name_with_path, red_strerror(red_errno));
        return -1;
    }

    if (offset != 0) {
        rc = red_lseek(fp, offset, RED_SEEK_SET);
        if (rc == -1) {
            debug_print("Unable to seek %s  to offset %d: %s\n", file_name_with_path, offset, red_strerror(red_errno));

            rc = red_close(fp);
            if (rc != 0) {
                printf("Unable to close %s: %s\n", file_name_with_path, red_strerror(red_errno));
            }
            return -1;
        }
    }

    int32_t numOfBytesRead = red_read(fp, read_buffer, length);
    if (numOfBytesRead == -1) {
        debug_print("Unable to read %s: %s\n", file_name_with_path, red_strerror(red_errno));
    }

    rc = red_close(fp);
    if (rc != 0) {
        printf("Unable to close %s: %s\n", file_name_with_path, red_strerror(red_errno));
    }
    return numOfBytesRead;
}

/**
 * Get the size of a file.
 *
 * Returns the size of the file or -1 if there is an error.
 *
 */
int32_t dir_fs_get_file_size(char *file_name_with_path) {
    int32_t rc;
    int64_t numOfBytesRead; // we need room for a 32 bit size and a negative number for an error.  TODO - we could limit to u32 bits if we never have files over 31 bits in size

    int32_t fp = red_open(file_name_with_path, RED_O_RDONLY);
    if (fp == -1) {
        debug_print("Unable to open %s for reading: %s\n", file_name_with_path, red_strerror(red_errno));
        return -1;
    }

    numOfBytesRead = red_lseek(fp, 0, RED_SEEK_END);
    if (numOfBytesRead == -1) {
        debug_print("Unable to seek %s to end: %s\n", file_name_with_path, red_strerror(red_errno));

        rc = red_close(fp);
        if (rc != 0) {
            printf("Unable to close %s: %s\n", file_name_with_path, red_strerror(red_errno));
        }
        return -1;
    }

    rc = red_close(fp);
    if (rc != 0) {
        printf("Unable to close %s: %s\n", file_name_with_path, red_strerror(red_errno));
    }
    return numOfBytesRead;
}

#ifdef USE_MRAM_TEST_FILESYSTEM_HACK
/**
 * SIMPLE MRAM FILE SYSTEM FOLLOWS
 */


/**
 * Read a record from the file allocation table
 */
bool dir_mram_get_node(uint32_t file_handle, MRAM_FILE * dir_node) {
    bool rc = readNV(dir_node, sizeof(MRAM_FILE),NVConfigData, (int)&(LocalFlash->MRAMFiles[file_handle]));
    if (!rc) {
        debug_print("MRAM FAT read - FAILED\n");
        return FALSE;
    }
    return TRUE;
}

/**
 * This saves data to a file based on the file id.  There is no automatic management of
 * space by this function, so we must know exactly where the data goes in the FAT and in the MRAM!
 *
 */
bool dir_mram_write_file(uint32_t file_handle, uint8_t *data, uint32_t length, uint32_t file_id, uint32_t upload_time,
                         uint16_t body_offset, uint32_t address) {

    bool rc;
    rc = writeNV(&file_handle,sizeof(uint32_t),NVConfigData,(int)&(LocalFlash->MRAMFiles[file_handle].file_handle));
        if (!rc) {  debug_print("Write MRAM FAT file_handle - FAILED\n");
            return FALSE; }
    rc = writeNV(&file_id,sizeof(uint32_t),NVConfigData,(int)&(LocalFlash->MRAMFiles[file_handle].file_id));
    if (!rc) {  debug_print("Write MRAM FAT file_id - FAILED\n");
        return FALSE; }
    rc = writeNV(&length,sizeof(uint32_t),NVConfigData,(int)&(LocalFlash->MRAMFiles[file_handle].file_size));
    if (!rc) {  debug_print("Write MRAM FAT file_size - FAILED\n");
        return FALSE; }
    rc = writeNV(&address,sizeof(uint32_t),NVConfigData,(int)&(LocalFlash->MRAMFiles[file_handle].address));
    if (!rc) {  debug_print("Write MRAM FAT address - FAILED\n");
        return FALSE; }
    rc = writeNV(&upload_time,sizeof(uint32_t),NVConfigData,(int)&(LocalFlash->MRAMFiles[file_handle].upload_time));
    if (!rc) {  debug_print("Write MRAM FAT upload_time - FAILED\n");
        return FALSE; }
    rc = writeNV(&body_offset,sizeof(uint16_t),NVConfigData,(int)&(LocalFlash->MRAMFiles[file_handle].body_offset));
    if (!rc) {  debug_print("Write MRAM FAT body_offset - FAILED\n"); return FALSE; }

    rc = writeNV(data,length,NVConfigData,(int)address);
    if (!rc) {  debug_print("Write MRAM file data - FAILED\n");
        return FALSE; }
    return TRUE;
}

bool dir_mram_append_to_file(uint32_t file_handle, uint8_t *data, uint32_t length ) {

    bool rc;
    uint32_t address;
    uint32_t size;

    // Read the address for the start of the file
    rc = readNV(&address, sizeof(uint32_t),NVConfigData, (int)&(LocalFlash->MRAMFiles[file_handle].address));
    if (!rc) {  debug_print("Write MRAM FAT header - FAILED\n"); return FALSE; }

    // Read the existing size
    rc = readNV(&size, sizeof(uint32_t),NVConfigData, (int)&(LocalFlash->MRAMFiles[file_handle].file_size));
    if (!rc) {  debug_print("Write MRAM FAT header - FAILED\n"); return FALSE; }

    // Append the data
    rc = writeNV(data,length,NVConfigData,(int)address+size);
    if (!rc) {  debug_print("Write MRAM file data - FAILED\n"); return FALSE; }

    // Write the new length
    size = size + length;
    rc = writeNV(&size,sizeof(uint32_t),NVConfigData,(int)&(LocalFlash->MRAMFiles[file_handle].file_size));
    if (!rc) {  debug_print("Write MRAM FAT header - FAILED\n"); return FALSE; }

    return TRUE;
}

/**
 *  A simple (and not very safe) routine to write a chunk from MRAM
 *  Undefined if you write too much or outside the chip!
 *  If the data written extends the size of the file then the new size is stored in
 *  the header
 */
bool dir_mram_write_file_chunk(MRAM_FILE *mram_file, uint8_t *data, uint32_t chunk_length, uint32_t offset) {

    bool rc;
    uint32_t size;
    // Write the data
    rc = writeNV(data,chunk_length,NVConfigData,(int)(mram_file->address + offset));
    if (!rc) {  debug_print("Write MRAM file data - FAILED\n"); return FALSE; }

    // If this extends the file then write the new length
    if (offset+chunk_length > mram_file->file_size) {
        size = offset + chunk_length;
        rc = writeNV(&size,sizeof(uint32_t),NVConfigData,(int)&(LocalFlash->MRAMFiles[mram_file->file_handle].file_size));
        mram_file->file_size = size;
    if (!rc) {  debug_print("Write MRAM FAT header - FAILED\n"); return FALSE; }
    }
    return TRUE;
}

/**
 *  A simple (and not very safe) routine to read a chunk from MRAM
 *  Undefined if you read too much or outside the chip!
 */
bool dir_mram_read_file_chunk(MRAM_FILE *mram_file, uint8_t *data, uint32_t chunk_length, uint32_t offset) {
    bool rc;

    if (mram_file->file_size < offset + chunk_length) {
        debug_print("Read MRAM File size shorter than read amount. Read FAILED\n");
        return FALSE;
    }
    // Read the data
    rc = readNV(data, chunk_length, NVConfigData, (int)(mram_file->address + offset));
    if (!rc) {
        debug_print("Read MRAM File data header - FAILED\n");
        return FALSE;
    }

    return TRUE;
}
#endif

#ifdef DEBUG

/**
 * TEST ROUTINES FOLLOW
 *
 */

/**
 * Test the Pacsat dir.  The command "make psf" needs to have been run already to generate the
 * test files.
 *
 */
int test_pacsat_dir() {
    printf("##### TEST PACSAT DIR:\n");
    int rc = EXIT_SUCCESS;
    debug_print("TEST DIR LOAD\n");
    dir_load();
    dir_debug_print(dir_head);
    if (dir_head->file_id != 1) { printf("** Error creating file 1\n"); return EXIT_FAILURE; }
    if (dir_head->next->file_id != 2) { printf("** Error creating file 2\n"); return EXIT_FAILURE; }
    if (dir_tail->file_id != 4) { printf("** Error creating file 4\n"); return EXIT_FAILURE; }

#ifdef REFACTOR
    debug_print("DELETE HEAD\n");
    dir_delete_node(dir_head);
    dir_debug_print(dir_head);
    if (dir_head->pfh->fileId != 2) { printf("** Error deleting head with file 2\n"); return EXIT_FAILURE; }
    if (dir_head->next->pfh->fileId != 3) { printf("** Error deleting head with file 3\n"); return EXIT_FAILURE; }
    dir_free();

    if (make_three_test_entries() == EXIT_FAILURE) { printf("** Could not make test files\n"); return EXIT_FAILURE; }
    debug_print("DELETE MIDDLE\n");
    dir_delete_node(dir_head->next);
    dir_debug_print(dir_head);
    if (dir_head->pfh->fileId != 1) { printf("** Error deleting middle with file 1\n"); return EXIT_FAILURE; }
    if (dir_head->next->pfh->fileId != 3) { printf("** Error deleting middle with file 3\n"); return EXIT_FAILURE; }
    dir_free();

    if (make_three_test_entries() == EXIT_FAILURE) { printf("** Could not make test files\n"); return EXIT_FAILURE; }
    debug_print("DELETE TAIL\n");
    dir_delete_node(dir_tail);
    dir_debug_print(dir_head);
    if (dir_head->pfh->fileId != 1) { printf("** Error deleting tail with file 1\n"); return EXIT_FAILURE; }
    if (dir_head->next->pfh->fileId != 2) { printf("** Error deleting tail with file 2\n"); return EXIT_FAILURE; }

    dir_free();

    if (make_three_test_entries() == EXIT_FAILURE) { printf("** Could not make test files\n"); return EXIT_FAILURE; }
    dir_debug_print(dir_head);
    dir_free(); // we just want the fresh files on disk, but the dir list to be empty

    /* Now load the dir from the folder and check that it is the same
     * This tests insert to blank list, insert at end and insert in the middle, assuming
     * the load order is file1, file3, file2
     */
    debug_print("LOAD DIR\n");
    //dir_load();
    if (dir_load_pacsat_file("/tmp/test_dir/0001.act") != EXIT_SUCCESS) {  printf("** Could not load psf 1\n"); return EXIT_FAILURE; }
    if (dir_load_pacsat_file("/tmp/test_dir/0002.act") != EXIT_SUCCESS) {  printf("** Could not load psf 2\n"); return EXIT_FAILURE; }
    if (dir_load_pacsat_file("/tmp/test_dir/0003.act") != EXIT_SUCCESS) {  printf("** Could not load psf 3\n"); return EXIT_FAILURE; }
    if (dir_load_pacsat_file("/tmp/test_dir/0004.act") != EXIT_SUCCESS) {  printf("** Could not load psf 4\n"); return EXIT_FAILURE; }

    if (dir_head == NULL) {printf("** Could not load head\n"); return EXIT_FAILURE; }
    if (dir_head->next == NULL) {printf("** Could not load head + 1\n"); return EXIT_FAILURE; }
    if (dir_tail == NULL) {printf("** Could not load fail\n"); return EXIT_FAILURE; }

    if (dir_head->pfh->fileId != 1) { printf("** Error loading file 1 as head\n"); return EXIT_FAILURE; }
    if (dir_head->next->pfh->fileId != 2) { printf("** Error loading file 2 as second entry\n"); return EXIT_FAILURE; }
    if (dir_tail->pfh->fileId != 4) { printf("** Error loading file 4 as tail\n"); return EXIT_FAILURE; }
    debug_print("LOADED DIR LIST\n");
    dir_debug_print(dir_head);
    debug_print("TEST DUPLICATE DIR LOAD - expecting load errors, but exit success\n");
    if (dir_load() != EXIT_SUCCESS) { printf("** Error testing duplicate insertion\n"); return EXIT_FAILURE; } // confirm duplicates not loaded

    /* Test search for file */
    if (dir_get_node_by_id(1) == NULL) { printf("** Error finding file 1\n"); return EXIT_FAILURE; }
    DIR_NODE * last = dir_get_node_by_id(4);
    if ( last == NULL) { printf("** Error finding file 4\n"); return EXIT_FAILURE; }
    if ( last->next != NULL) { printf("** Error duplicate insert after file 4\n"); return EXIT_FAILURE; }
    if (dir_get_node_by_id(9999) != NULL) { printf("** Error with search for missing file\n"); return EXIT_FAILURE; }

#endif

    if (rc == EXIT_SUCCESS)
        printf("##### TEST PACSAT DIR: success\n");
    else
        printf("##### TEST PACSAT DIR: fail\n");
    return rc;
}

#endif /* DEBUG */
