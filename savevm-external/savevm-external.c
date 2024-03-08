#include "qemu/osdep.h"

#include <gio/gio.h>

// #include <archive.h>
// #include <archive_entry.h>

#include "block/block_int-io.h"
#include "block/block-io.h"
#include "block/snapshot.h"
#include "io/channel-buffer.h"
#include "io/channel-file.h"
#include "migration/global_state.h"
#include "migration/migration.h"
#include "migration/qemu-file.h"
#include "migration/savevm.h"
#include "monitor/hmp.h"
#include "monitor/monitor.h"
#include "qapi/error.h"
#include "qapi/qapi-builtin-visit.h"
#include "qapi/qapi-commands-block.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qerror.h"
#include "qapi/util.h"
#include "qemu/cutils.h"
#include "qemu/main-loop.h"
#include "qemu/typedefs.h"
#include "qemu/log.h"
#include "sysemu/replay.h"
#include "sysemu/runstate.h"





#ifdef CONFIG_LIBQFLEX
#include "middleware/libqflex/legacy-qflex-api.h"
#include "middleware/libqflex/libqflex-module.h"
#endif

#include "snapvm-external.h"


/**
 * Iterate over block driver's backing file chain until reaching
 * the firstmost block driver
 *
 * Iterrative version of the following code jff vvv
 *-- static BlockDriverState*
 *-- get_base_bdrv(BlockDriverState* bs_root) {
 *--     if (!bs_root || !bs_root->backing)
 *--         return bs_root;
 *--     return get_base_bdrv(child_bs(bs_root->backing));
 *-- }
 */

static BlockDriverState*
get_base_bdrv(BlockDriverState* bs_root) {
    BlockDriverState* bs = bs_root;
    while (bs && bs->backing) {
        bs = child_bs(bs->backing);
    }
    return bs;
}

/**
 *? Make all block drive filepath *point* to the same location
 */
static void
bdrv_update_filename(BlockDriverState* bs, char const * const path) {
    pstrcpy(bs->filename, sizeof(bs->filename), path);
    pstrcpy(bs->exact_filename, sizeof(bs->exact_filename), path);
    pstrcpy(bs->file->bs->filename, sizeof(bs->filename), path);
    pstrcpy(bs->file->bs->exact_filename, sizeof(bs->exact_filename), path);
    bdrv_refresh_filename(bs);
}

//TODO refactoring needed here
// static void
// get_snap_mem_file_dir(
//     char const * const base_filename,
//     char const * const snap_name,
//     GString* snap_file_dir,
//     int const checkpoint_num)
// {
//     char* dirpath = base_filename ? g_path_get_dirname(base_filename) : (char *)(".");

//     if (checkpoint_num >= 0) {
//         g_string_append_printf(snap_file_dir, "%s/%s/checkpoint_%i/mem",
//                                 dirpath, snap_name, checkpoint_num);
//     } else {
//         g_string_append_printf(snap_file_dir, "%s/%s/mem", dirpath, snap_name);
//     }
//     g_mkdir_with_parents(g_path_get_dirname(snap_file_dir->str), 0700);

// }


/**
 * There is no name, therefor it's an increment
 * of the previous disk image.
 *
 * If it has been loaded before, put it asside of the root,
 * otherwise put it in a tmp folder
 */
static void
save_bdrv_new_increment(
    SnapTransaction* trans,
    Error** errp
)
{

    /**
     * Take the filename of the root image, and append the datetime to it
     */
    trans->datetime = get_datetime();
    g_autoptr(GString) snap_path_dst = g_string_new("");
    g_autoptr(GString) new_filename_buf = g_string_new("");

    g_string_append_printf(
        new_filename_buf,
        "%s-%s",
        trans->root_bdrv.basename,
        trans->datetime);

    trans->new_bdrv.basename = new_filename_buf->str;

    char const * const format = qemu_snapvm_ext_state.has_been_loaded ? "%s/%s" : "%s/tmp/%s";

    g_string_append_printf(
        snap_path_dst,
        format,
        trans->root_bdrv.fullpath,
        trans->new_bdrv.basename);

    trans->new_bdrv.fullpath =  g_string_free(snap_path_dst, false);

    // ─── Create Or Update Previous Snapshot ──────────────────────────────

    char const * const device_name = bdrv_get_device_name(trans->root_bdrv.bs);

    qmp_blockdev_snapshot_sync(
        device_name,
        NULL,
        trans->new_bdrv.fullpath,
        NULL,
        "qcow2",
        true,
        NEW_IMAGE_MODE_ABSOLUTE_PATHS,
        errp);


    trans->new_bdrv.bs = bdrv_lookup_bs(device_name, NULL, errp);

    //? Relative backing file... maybe
    // g_autoptr(GFile) bs_gfile           =  g_file_new_for_path(root_bdrv_dirname);
    // g_autoptr(GFile) bs_backing_gfile   =  g_file_new_for_path(snap_path_dst->str);
    // g_autofree char* backing_rel_path = g_file_get_relative_path(bs_gfile, bs_backing_gfile);
    // qemu_log("%s",backing_rel_path);
    // pstrcpy(bs_new->backing_file, sizeof(bs_new->backing_file), backing_rel_path);

    bdrv_update_filename(trans->new_bdrv.bs, trans->new_bdrv.fullpath);
}

/**
 * Create a new root external snapshot.
 * A name is passed by and a copy of the disk
 * should be made to avoid altering the original disk
 */
static void
save_bdrv_new_root(
    SnapTransaction* trans,
    Error** errp
    )
{

    g_assert(trans->root_bdrv.dirname);
    g_assert(trans->root_bdrv.basename);

    trans->new_bdrv.basename = trans->root_bdrv.basename;
    trans->new_bdrv.dirname  = g_build_path(G_DIR_SEPARATOR_S, trans->root_bdrv.dirname, trans->new_name, NULL);
    trans->new_bdrv.fullpath = g_build_path(G_DIR_SEPARATOR_S, trans->new_bdrv.dirname, trans->root_bdrv.basename, NULL);

    g_mkdir_with_parents(trans->new_bdrv.dirname , 0700);

    if (link(trans->root_bdrv.fullpath, trans->new_bdrv.fullpath) < 0)
    {
        error_setg(
            errp,
            "Could not move external snapshot bdrv (block device) %s to %s",
            trans->root_bdrv.fullpath,
            trans->new_bdrv.fullpath);
    }


    trans->new_bdrv.bs = trans->root_bdrv.bs;
    bdrv_update_filename(trans->new_bdrv.bs, trans->new_bdrv.fullpath);

    // Consider that know we have loaded a snapshot and all next
    // logic should behave accordingly.
    qemu_snapvm_ext_state.has_been_loaded = true;
}

/**
 * Save device/block drive state into a full snapshot
 * or an incremental snapshot.
 *
 * Case 1: A name is passed by and a copy of the disk should be
 * made.
 *
 * Case 2: There is no name, therefor it's an increment
 * of the previous disk image.
 *      Sub Case 1: The root image is still the very base image
 *          -> add increment in 'tmp' dir
 *      Sub Case 2: The root image is a previously full snapshot save (Case 1)
 *                  and the check point have to be incremented in the dir
 * TODO: handle relative backing file path
 */
static bool
save_snapshot_external_bdrv(
    SnapTransaction* trans,
    Error** errp)
{

    switch (trans->mode)
    {
    case INCREMENT:
        save_bdrv_new_increment(trans, errp);
        g_assert(trans->datetime != NULL);
        break;

    case NEW_ROOT:
        g_assert(trans->new_name != NULL);
        save_bdrv_new_root(trans, errp);
        break;

    default:
        g_assert_not_reached();
    }


    if (*errp)
    {
        error_report_err(*errp);
        return false;
    }

    return true;

}

static bool
save_snapshot_external_mem(
    SnapTransaction const * trans,
    Error **errp)
{
    switch(trans->mode)
    {
        case NEW_ROOT:
        break;

        case INCREMENT:

        break;

        default:
        g_assert_not_reached();
    }
    return true;
}

//     // g_autoptr(GString) new_mem_path     = g_string_new("");
//     g_autoptr(GString) new_mem_filename = g_string_new("");


//     //! THIS IS SKETCHY the SNAP NAME vs NO NAME should be splitted when
//     //! the hmp/qmp function is called
//     if (snap_name)
//     {
//         g_assert(snap_name != NULL && (datetime == NULL));
//         // save no time stamp
//         g_string_append_printf(new_mem_filename, "mem");
//     }
//     else
//     {
//         // save w/ timestamp
//         g_assert(datetime && (snap_name == NULL));
//         g_string_append_printf(new_mem_filename, "mem-%s", datetime);
//         g_free(datetime); //! VERY Sketchy
//     }


//     g_autofree char const * bdrv_basename = g_path_get_dirname(brdv_path);
//     g_autofree char const * new_mem_path = g_build_path(
//                                                 G_DIR_SEPARATOR_S,
//                                                 bdrv_basename,
//                                                 new_mem_filename,
//                                                 NULL);

//     // get_snap_mem_file_dir(brdv_path, snap_name, new_mem_filename);

//     QIOChannelFile* ioc = qio_channel_file_new_path(
//         new_mem_path,
//         O_WRONLY | O_CREAT | O_TRUNC,
//         0660,
//         errp);

//     qio_channel_set_name(QIO_CHANNEL(ioc), "snapvm-external-memory-outgoing");

//     if (!ioc)
//     {
//         error_setg(errp, "Could not open channel");
//         goto end;
//     }

//     QEMUFile* f = qemu_file_new_output(QIO_CHANNEL(ioc));

//     if (!f)
//     {
//         error_setg(errp, "Could not open VM state file");
//         goto end;
//     }

//     object_unref(OBJECT(ioc));

//     int ret = qemu_savevm_state(f, errp);
//     // Perfrom noflush and return total number of bytes transferred
//     qemu_file_transferred(f);

//     if (ret < 0 || qemu_fclose(f) < 0)
//         error_setg(errp, QERR_IO_ERROR);

// end:

//     if (*errp)
//     {
//         error_report_err(*errp);
//         return false;
//     }

//     return true;


// }

bool save_snapshot_external(
    SnapTransaction* trans,
    Error** errp)
{

    bool ret = true;
    g_autoptr(GList) bdrvs = NULL;
    RunState saved_state = runstate_get();

    // Make sure that we are in the main thread, and not
    // concurrently accessing the ressources
    GLOBAL_STATE_CODE();

    // ─── Vm State Checkup ────────────────────────────────────────────────

    /**
     * Internal snapshot do not allow this, therefor we repliacte this behaviour
     */
    if (migration_is_blocked(errp))
    {
        ret =  false;
        goto end;
    }

    if (!replay_can_snapshot())
    {
        error_setg(errp, "Record/replay does not allow making snapshot "
                   "right now. Try once more later.");

        ret = false;
        goto end;
    }

    //? Test if all drive are snapshot-able
    if (!bdrv_all_can_snapshot(false, NULL, errp)) { return false; }

    trans->root_bdrv.bs = bdrv_all_find_vmstate_bs(NULL, false, NULL, errp);
    if (trans->root_bdrv.bs == NULL)
    {
        ret =  false;
        goto end;
    }

    // Retrieve all snaspshot able drive
    if (bdrv_all_get_snapshot_devices( false, NULL, &bdrvs, errp))
    {
        ret =  false;
        goto end;
    }

    // No stupid behaviour
    g_assert(bdrvs != NULL);

    vm_stop(RUN_STATE_SAVE_VM);
    // Trigger the global state to be saved within the snapshot
    global_state_store();


    // ─── Start Snapshotting Procedure ────────────────────────────────────


    /* If needed */
    trans->root_bdrv.fullpath   = get_base_bdrv(trans->root_bdrv.bs)->filename;
    trans->root_bdrv.dirname    = g_path_get_dirname(trans->root_bdrv.fullpath);
    trans->root_bdrv.basename   = g_path_get_basename(trans->root_bdrv.fullpath);
    trans->datetime             = get_datetime();

    // ─── Iterate And Save BDRV ───────────────────────────────────────────

    GList* iterbdrvs = bdrvs;

    while (iterbdrvs) {
        trans->root_bdrv.bs = iterbdrvs->data;
        if (!bdrv_is_read_only(trans->root_bdrv.bs)) {
            ret = save_snapshot_external_bdrv(trans, errp);

            if (!ret) goto end;
            trans->new_bdrv.fullpath = trans->root_bdrv.bs->filename;
        }
        iterbdrvs = iterbdrvs->next;
    }

//     // ─── Saving Main Memory ──────────────────────────────────────────────

    ret = save_snapshot_external_mem(trans, errp);

//     if (!ret) goto end;

// #ifdef CONFIG_LIBQFLEX
//     if (qemu_libqflex_state.is_initialised) {
//         g_autoptr(GString) dst_path = g_string_new("");

//         g_autofree char* base_bdrv_dirname = g_path_get_dirname(new_bdrv_path);
//         g_string_append_printf(dst_path, "%s/%s", base_bdrv_dirname, snap_name);

//         if (checkpoint_num != -1)
//             g_string_append_printf(dst_path, "/checkpoint_%i", checkpoint_num);

//         // call flexus API to trigger the cache memory dump
//         flexus_api.qmp(QMP_FLEXUS_DOSAVE, dst_path->str);
//     }
// #endif

    // ─── Return Gracefully Or... Not ─────────────────────────────────────

end:

    //? Allow to start the vm again only if no error was detected
    if (!ret)
        error_report_err(*errp);
    else
        vm_resume(saved_state);

    return ret;
}

// ─────────────────────────────────────────────────────────────────────────────