#include <sqfs/predef.h>
#include <glib.h>
#include <unistd.h>
#include <glib/gchecksum.h>
#include <fcntl.h>
#include <sqfs/frag_table.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sqfs/block.h>
#include <stdio.h>
#include <sqfs/super.h>
#include <sqfs/io.h>
#include <sqfs/error.h>
#include <sqfs/id_table.h>
#include <sqfs/dir.h>
#include <sqfs/dir_reader.h>
#include <sqfs/data_reader.h>
#include <sqfs/compressor.h>

// TODO: copied from data_reader.c
struct sqfs_data_reader_t {
	sqfs_object_t obj;

	sqfs_frag_table_t *frag_tbl;
	sqfs_compressor_t *cmp;
	sqfs_file_t *file;

	sqfs_u8 *data_block;
	size_t data_blk_size;
	sqfs_u64 current_block;

	sqfs_u8 *frag_block;
	size_t frag_blk_size;
	sqfs_u32 block_size;

	sqfs_u8 scratch[];
};


typedef struct {
	sqfs_compressor_config_t cfg;
	sqfs_compressor_t *cmp;
	sqfs_super_t super;
	sqfs_file_t *file;
	sqfs_id_table_t *idtbl;
	sqfs_dir_reader_t *dr;
	sqfs_tree_node_t *root;
	sqfs_data_reader_t *data;

	sqfs_compressor_config_t options;
    GChecksum *checksum;
    void *file_map;
} sqfs_state_t;

void sqfs_perror(const char *file, const char *action, int error_code)
{
	const char *errstr;

	switch (error_code) {
	case SQFS_ERROR_ALLOC:
		errstr = "out of memory";
		break;
	case SQFS_ERROR_IO:
		errstr = "I/O error";
		break;
	case SQFS_ERROR_COMPRESSOR:
		errstr = "internal compressor error";
		break;
	case SQFS_ERROR_INTERNAL:
		errstr = "internal error";
		break;
	case SQFS_ERROR_CORRUPTED:
		errstr = "data corrupted";
		break;
	case SQFS_ERROR_UNSUPPORTED:
		errstr = "unknown or not supported";
		break;
	case SQFS_ERROR_OVERFLOW:
		errstr = "numeric overflow";
		break;
	case SQFS_ERROR_OUT_OF_BOUNDS:
		errstr = "location out of bounds";
		break;
	case SFQS_ERROR_SUPER_MAGIC:
		errstr = "wrong magic value in super block";
		break;
	case SFQS_ERROR_SUPER_VERSION:
		errstr = "wrong squashfs version in super block";
		break;
	case SQFS_ERROR_SUPER_BLOCK_SIZE:
		errstr = "invalid block size specified in super block";
		break;
	case SQFS_ERROR_NOT_DIR:
		errstr = "target is not a directory";
		break;
	case SQFS_ERROR_NO_ENTRY:
		errstr = "no such file or directory";
		break;
	case SQFS_ERROR_LINK_LOOP:
		errstr = "hard link loop detected";
		break;
	case SQFS_ERROR_NOT_FILE:
		errstr = "target is not a file";
		break;
	case SQFS_ERROR_ARG_INVALID:
		errstr = "invalid argument";
		break;
	case SQFS_ERROR_SEQUENCE:
		errstr = "illegal oder of operations";
		break;
	default:
		errstr = "libsquashfs returned an unknown error code";
		break;
	}

	if (file != NULL)
		fprintf(stderr, "%s: ", file);

	if (action != NULL)
		fprintf(stderr, "%s: ", action);

	fprintf(stderr, "%s.\n", errstr);
}


static int open_sfqs(sqfs_state_t *state, const char *path)
{
	int ret;

	state->file = sqfs_open_file(path, SQFS_FILE_OPEN_READ_ONLY);
	if (state->file == NULL) {
		perror(path);
		return -1;
	}

	ret = sqfs_super_read(&state->super, state->file);
	if (ret) {
		sqfs_perror(path, "reading super block", ret);
		goto fail_file;
	}

	sqfs_compressor_config_init(&state->cfg, state->super.compression_id,
				    state->super.block_size,
				    SQFS_COMP_FLAG_UNCOMPRESS);

	ret = sqfs_compressor_create(&state->cfg, &state->cmp);

#ifdef WITH_LZO
	if (state->super.compression_id == SQFS_COMP_LZO && ret != 0)
		ret = lzo_compressor_create(&state->cfg, &state->cmp);
#endif

	if (ret != 0) {
		sqfs_perror(path, "creating compressor", ret);
		goto fail_file;
	}

	if (state->super.flags & SQFS_FLAG_COMPRESSOR_OPTIONS) {
		ret = state->cmp->read_options(state->cmp, state->file);

		if (ret == 0) {
			state->cmp->get_configuration(state->cmp,
						      &state->options);
		} else {
			sqfs_perror(path, "reading compressor options", ret);
		}
	}

	state->idtbl = sqfs_id_table_create(0);
	if (state->idtbl == NULL) {
		sqfs_perror(path, "creating ID table", SQFS_ERROR_ALLOC);
		goto fail_cmp;
	}

	ret = sqfs_id_table_read(state->idtbl, state->file,
				 &state->super, state->cmp);
	if (ret) {
		sqfs_perror(path, "loading ID table", ret);
		goto fail_id;
	}

	state->dr = sqfs_dir_reader_create(&state->super, state->cmp,
					   state->file, 0);
	if (state->dr == NULL) {
		sqfs_perror(path, "creating directory reader",
			    SQFS_ERROR_ALLOC);
		goto fail_id;
	}

	ret = sqfs_dir_reader_get_full_hierarchy(state->dr, state->idtbl,
						 NULL, 0, &state->root);
	if (ret) {
		sqfs_perror(path, "loading filesystem tree", ret);
		goto fail_dr;
	}

	state->data = sqfs_data_reader_create(state->file,
					      state->super.block_size,
					      state->cmp, 0);
	if (state->data == NULL) {
		sqfs_perror(path, "creating data reader", SQFS_ERROR_ALLOC);
		goto fail_tree;
	}

	ret = sqfs_data_reader_load_fragment_table(state->data, &state->super);
	if (ret) {
		sqfs_perror(path, "loading fragment table", ret);
		goto fail_data;
	}

	return 0;
fail_data:
	sqfs_destroy(state->data);
fail_tree:
	sqfs_dir_tree_destroy(state->root);
fail_dr:
	sqfs_destroy(state->dr);
fail_id:
	sqfs_destroy(state->idtbl);
fail_cmp:
	sqfs_destroy(state->cmp);
fail_file:
	sqfs_destroy(state->file);
	return -1;
}

/* static int iter_over_tree(struct sqfs_state_t *state) */
/* { */
/*  	sqfs_tree_node_t *root, *tail, *new; */
/* 	sqfs_inode_generic_t *inode; */
/* 	sqfs_dir_entry_t *ent; */
/* 	const char *ptr; */
/* 	int ret; */

/* 	if (flags & ~SQFS_TREE_ALL_FLAGS) */
/* 		return SQFS_ERROR_UNSUPPORTED; */

/* 	ret = sqfs_dir_reader_get_root_inode(rd, &inode); */
/* 	if (ret) */
/* 		return ret; */

/*     ret = sqfs_dir_reader_open_dir(rd, tail->inode, */
/*                                    SQFS_DIR_OPEN_NO_DOT_ENTRIES); */
/*     if (ret) */
/*         goto fail; */


/* fail: */
/*     fprintf(stderr, "iter_over_tree"); */
/*     return -1; */

/* } */


sqfs_state_t *shim_open(const char *path)
{
    sqfs_state_t *state = calloc(1, sizeof(*state));

    if (open_sfqs(state, path)) {
        fprintf(stderr, "open_sfqs");
        return NULL;
    };
    state->checksum = g_checksum_new(G_CHECKSUM_SHA256);

    //TODO I hate opening the same file twice, but I don'd want to import the sqfs_file_stdio_t
    int fd = open(path, O_RDONLY);
    struct stat sb;
    fstat(fd, &sb);
    state->file_map = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (state->file_map == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    close(fd);
    return state;
}

int shim_lookup(sqfs_state_t *state, uint32_t index, unsigned char hash[100], uint64_t *offset, uint64_t *size_ret)
{

    sqfs_fragment_t frag;
    if (sqfs_frag_table_lookup(state->data->frag_tbl, index, &frag) ) {
        return -1;
    }

    sqfs_u32 size = frag.size;
    if (SQFS_IS_SPARSE_BLOCK(size)) {
        printf("fragment %u: sparse\n", index);
        return -1;
    }

    sqfs_u32 on_disk_size = SQFS_ON_DISK_BLOCK_SIZE(size);
    g_checksum_update(state->checksum, (guchar *)state->file_map + frag.start_offset, on_disk_size);

    strlcpy(hash, g_checksum_get_string(state->checksum), 100);
    *offset = frag.start_offset;
    *size_ret = on_disk_size;
    g_checksum_reset(state->checksum);
    return 0;
}


int shim_close(sqfs_state_t *state) {
    //TODO
    //     g_checksum_free(checksum);

}

int shim_get_data_offsets(sqfs_state_t *state, uint64_t *start, uint64_t *end)
{
    *start = sizeof(sqfs_super_t)+sizeof(sqfs_u32);
    *end = state->super.inode_table_start;
    return 0;
}
