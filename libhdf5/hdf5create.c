/* Copyright 2003-2018, University Corporation for Atmospheric
 * Research. See COPYRIGHT file for copying and redistribution
 * conditions. */
/**
 * @file
 * @internal The netCDF-4 file functions relating to file creation.
 *
 * @author Ed Hartnett
 */

#include "config.h"
#include "hdf5internal.h"

/* From hdf5file.c. */
extern size_t nc4_chunk_cache_size;
extern size_t nc4_chunk_cache_nelems;
extern float nc4_chunk_cache_preemption;

/** @internal These flags may not be set for create. */
static const int ILLEGAL_CREATE_FLAGS = (NC_NOWRITE|NC_MMAP|NC_64BIT_OFFSET|NC_CDF5);

/* From nc4mem.c */
extern int NC4_create_image_file(NC_FILE_INFO_T* h5, size_t);

/**
 * @internal Create a netCDF-4/HDF5 file.
 *
 * @param path The file name of the new file.
 * @param cmode The creation mode flag.
 * @param initialsz The proposed initial file size (advisory)
 * @param parameters extra parameter info (like  MPI communicator)
 * @param nc Pointer to an instance of NC.
 *
 * @return ::NC_NOERR No error.
 * @return ::NC_EINVAL Invalid input (check cmode).
 * @return ::NC_EEXIST File exists and NC_NOCLOBBER used.
 * @return ::NC_EHDFERR HDF5 returned error.
 * @ingroup netcdf4
 * @author Ed Hartnett, Dennis Heimbigner
 */
static int
nc4_create_file(const char *path, int cmode, size_t initialsz,
                void* parameters, NC *nc)
{
    hid_t fcpl_id, fapl_id = -1;
    unsigned flags;
    FILE *fp;
    int retval = NC_NOERR;
    NC_FILE_INFO_T* nc4_info = NULL;
    NC_HDF5_FILE_INFO_T *hdf5_info;
    NC_HDF5_GRP_INFO_T *hdf5_grp;

#ifdef USE_PARALLEL4
    NC_MPI_INFO *mpiinfo = NULL;
    MPI_Comm comm;
    MPI_Info info;
    int comm_duped = 0; /* Whether the MPI Communicator was duplicated */
    int info_duped = 0; /* Whether the MPI Info object was duplicated */
#endif /* !USE_PARALLEL4 */

    assert(nc && path);
    LOG((3, "%s: path %s mode 0x%x", __func__, path, cmode));

    /* Add necessary structs to hold netcdf-4 file data. */
    if ((retval = nc4_nc4f_list_add(nc, path, (NC_WRITE | cmode))))
        BAIL(retval);
    nc4_info = NC4_DATA(nc);
    assert(nc4_info && nc4_info->root_grp);
    nc4_info->root_grp->atts_read = 1;

    /* Add struct to hold HDF5-specific file metadata. */
    if (!(nc4_info->format_file_info = calloc(1, sizeof(NC_HDF5_FILE_INFO_T))))
        BAIL(NC_ENOMEM);
    hdf5_info = (NC_HDF5_FILE_INFO_T *)nc4_info->format_file_info;

    /* Add struct to hold HDF5-specific group info. */
    if (!(nc4_info->root_grp->format_grp_info = calloc(1, sizeof(NC_HDF5_GRP_INFO_T))))
        return NC_ENOMEM;
    hdf5_grp = (NC_HDF5_GRP_INFO_T *)nc4_info->root_grp->format_grp_info;

    nc4_info->mem.inmemory = (cmode & NC_INMEMORY) == NC_INMEMORY;
    nc4_info->mem.diskless = (cmode & NC_DISKLESS) == NC_DISKLESS;
    nc4_info->mem.persist =  (cmode & NC_PERSIST) == NC_PERSIST;
    nc4_info->mem.created = 1;
    nc4_info->mem.initialsize = initialsz;

    /* diskless => !inmemory */
    if(nc4_info->mem.inmemory && nc4_info->mem.diskless)
        BAIL(NC_EINTERNAL);

    if(nc4_info->mem.inmemory && parameters)
        nc4_info->mem.memio = *(NC_memio*)parameters;
#ifdef USE_PARALLEL4
    else if(parameters) {
        mpiinfo = (NC_MPI_INFO *)parameters;
        comm = mpiinfo->comm;
        info = mpiinfo->info;
    }
#endif
    if(nc4_info->mem.diskless)
        flags = H5F_ACC_TRUNC;
    else if(cmode & NC_NOCLOBBER)
        flags = H5F_ACC_EXCL;
    else
        flags = H5F_ACC_TRUNC;

    /* If this file already exists, and NC_NOCLOBBER is specified,
       return an error (unless diskless|inmemory) */
    if (!nc4_info->mem.diskless && !nc4_info->mem.inmemory) {
        if ((cmode & NC_NOCLOBBER) && (fp = fopen(path, "r"))) {
            fclose(fp);
            BAIL(NC_EEXIST);
        }
    }

    /* Need this access plist to control how HDF5 handles open objects
     * on file close. Setting H5F_CLOSE_SEMI will cause H5Fclose to
     * fail if there are any open objects in the file. */
    if ((fapl_id = H5Pcreate(H5P_FILE_ACCESS)) < 0)
        BAIL(NC_EHDFERR);
    if (H5Pset_fclose_degree(fapl_id, H5F_CLOSE_SEMI))
        BAIL(NC_EHDFERR);

#ifdef USE_PARALLEL4
    /* If this is a parallel file create, set up the file creation
       property list. */
    if (mpiinfo != NULL) {
        nc4_info->parallel = NC_TRUE;
        LOG((4, "creating parallel file with MPI/IO"));
        if (H5Pset_fapl_mpio(fapl_id, comm, info) < 0)
            BAIL(NC_EPARINIT);

        /* Keep copies of the MPI Comm & Info objects */
        if (MPI_SUCCESS != MPI_Comm_dup(comm, &nc4_info->comm))
            BAIL(NC_EMPI);
        comm_duped++;
        if (MPI_INFO_NULL != info)
        {
            if (MPI_SUCCESS != MPI_Info_dup(info, &nc4_info->info))
                BAIL(NC_EMPI);
            info_duped++;
        }
        else
        {
            /* No dup, just copy it. */
            nc4_info->info = info;
        }
    }
#else /* only set cache for non-parallel... */
    if (H5Pset_cache(fapl_id, 0, nc4_chunk_cache_nelems, nc4_chunk_cache_size,
                     nc4_chunk_cache_preemption) < 0)
        BAIL(NC_EHDFERR);
    LOG((4, "%s: set HDF raw chunk cache to size %d nelems %d preemption %f",
         __func__, nc4_chunk_cache_size, nc4_chunk_cache_nelems,
         nc4_chunk_cache_preemption));
#endif /* USE_PARALLEL4 */

#ifdef HAVE_H5PSET_LIBVER_BOUNDS
#if H5_VERSION_GE(1,10,2)
    if (H5Pset_libver_bounds(fapl_id, H5F_LIBVER_EARLIEST, H5F_LIBVER_V18) < 0)
#else
        if (H5Pset_libver_bounds(fapl_id, H5F_LIBVER_EARLIEST,
                                 H5F_LIBVER_LATEST) < 0)
#endif
            BAIL(NC_EHDFERR);
#endif

    /* Create the property list. */
    if ((fcpl_id = H5Pcreate(H5P_FILE_CREATE)) < 0)
        BAIL(NC_EHDFERR);

    /* RJ: this suppose to be FALSE that is defined in H5 private.h as 0 */
    if (H5Pset_obj_track_times(fcpl_id,0)<0)
        BAIL(NC_EHDFERR);

    /* Set latest_format in access propertly list and
     * H5P_CRT_ORDER_TRACKED in the creation property list. This turns
     * on HDF5 creation ordering. */
    if (H5Pset_link_creation_order(fcpl_id, (H5P_CRT_ORDER_TRACKED |
                                             H5P_CRT_ORDER_INDEXED)) < 0)
        BAIL(NC_EHDFERR);
    if (H5Pset_attr_creation_order(fcpl_id, (H5P_CRT_ORDER_TRACKED |
                                             H5P_CRT_ORDER_INDEXED)) < 0)
        BAIL(NC_EHDFERR);

#ifdef HDF5_HAS_COLL_METADATA_OPS
    /* If HDF5 supports collective metadata operations, turn them
     * on. This is only relevant for parallel I/O builds of HDF5. */
    if (H5Pset_all_coll_metadata_ops(fapl_id, 1) < 0)
        BAIL(NC_EHDFERR);
    if (H5Pset_coll_metadata_write(fapl_id, 1) < 0)
        BAIL(NC_EHDFERR);
#endif

    if(nc4_info->mem.inmemory) {
        retval = NC4_create_image_file(nc4_info,initialsz);
        if(retval)
            BAIL(retval);
    }
    else
        if(nc4_info->mem.diskless) {
            size_t alloc_incr;     /* Buffer allocation increment */
            size_t min_incr = 65536; /* Minimum buffer increment */
            double buf_prcnt = 0.1f;  /* Percentage of buffer size to set as increment */
            /* set allocation increment to a percentage of the supplied buffer size, or
             * a pre-defined minimum increment value, whichever is larger
             */
            if ((buf_prcnt * initialsz) > min_incr)
                alloc_incr = (size_t)(buf_prcnt * initialsz);
            else
                alloc_incr = min_incr;
            /* Configure FAPL to use the core file driver */
            if (H5Pset_fapl_core(fapl_id, alloc_incr, (nc4_info->mem.persist?1:0)) < 0)
                BAIL(NC_EHDFERR);
            if ((hdf5_info->hdfid = H5Fcreate(path, flags, fcpl_id, fapl_id)) < 0)
                BAIL(EACCES);
        }
        else /* Normal file */
        {
            /* Create the HDF5 file. */
            if ((hdf5_info->hdfid = H5Fcreate(path, flags, fcpl_id, fapl_id)) < 0)
                BAIL(EACCES);
        }

    /* Open the root group. */
    if ((hdf5_grp->hdf_grpid = H5Gopen2(hdf5_info->hdfid, "/", H5P_DEFAULT)) < 0)
        BAIL(NC_EFILEMETA);

    /* Release the property lists. */
    if (H5Pclose(fapl_id) < 0 || H5Pclose(fcpl_id) < 0)
        BAIL(NC_EHDFERR);

    /* Define mode gets turned on automatically on create. */
    nc4_info->flags |= NC_INDEF;

    /* Save the HDF5 superblock number and set the _NCProperties attribute. */
    if ((retval = NC4_new_provenance(nc4_info)))
        BAIL(retval);

    return NC_NOERR;

exit: /*failure exit*/
#ifdef USE_PARALLEL4
    if (comm_duped) MPI_Comm_free(&nc4_info->comm);
    if (info_duped) MPI_Info_free(&nc4_info->info);
#endif
    if (fapl_id != H5P_DEFAULT) H5Pclose(fapl_id);
    if(!nc4_info) return retval;
    nc4_close_hdf5_file(nc4_info, 1, NULL); /* treat like abort */
    return retval;
}

/**
 * @internal Create a netCDF-4/HDF5 file.
 *
 * @param path The file name of the new file.
 * @param cmode The creation mode flag.
 * @param initialsz Ignored by this function.
 * @param basepe Ignored by this function.
 * @param chunksizehintp Ignored by this function.
 * @param parameters pointer to struct holding extra data (e.g. for
 * parallel I/O) layer. Ignored if NULL.
 * @param dispatch Pointer to the dispatch table for this file.
 * @param nc_file Pointer to an instance of NC.
 *
 * @return ::NC_NOERR No error.
 * @return ::NC_EINVAL Invalid input (check cmode).
 * @ingroup netcdf4
 * @author Ed Hartnett
 */
int
NC4_create(const char* path, int cmode, size_t initialsz, int basepe,
           size_t *chunksizehintp, void *parameters,
           const NC_Dispatch *dispatch, NC *nc_file)
{
    int res;

    assert(nc_file && path);

    LOG((1, "%s: path %s cmode 0x%x parameters %p",
         __func__, path, cmode, parameters));

    /* If this is our first file, turn off HDF5 error messages. */
    if (!nc4_hdf5_initialized)
        nc4_hdf5_initialize();

#ifdef LOGGING
    /* If nc logging level has changed, see if we need to turn on
     * HDF5's error messages. */
    hdf5_set_log_level();
#endif /* LOGGING */

    /* Check the cmode for validity. */
    if((cmode & ILLEGAL_CREATE_FLAGS) != 0)
    {res = NC_EINVAL; goto done;}

    /* check parallel against NC_DISKLESS already done in NC_create() */

    nc_file->int_ncid = nc_file->ext_ncid;

    res = nc4_create_file(path, cmode, initialsz, parameters, nc_file);

done:
    return res;
}
