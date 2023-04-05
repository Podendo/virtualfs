#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>

#include <linux/fs.h>		/* libfs stuff is declared */
#include <linux/pagemap.h>	/* page cache size */
#include <asm/atomic.h>
#include <asm/uaccess.h>	/* copy to user */

#define LFS_MAGIC 0x11223344

/* @brief inode-representation for objects in dummyfs.
 * Anytime we make a file or directory in filesystem we need to come up
 * with an idone to represent it internally. This is the function that
 * does that job. All that`s really interesting is the "mode parameter,
 * which says whether this is a directory or file, and gives the permissions
 */
static struct inode *dummyfs_make_inode(struct super_block *sb, int mode,
		const struct file_operations *fops)
{
	struct inode *inode;

	inode = new_inode(sb);
	if(!inode)
		return NULL;

	inode->i_mode = mode;
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
	inode->i_fop = fops;
	inode->i_ino = get_next_ino();
	return inode;
}


static int dfs_open(struct inode *inode, struct file *fp)
{
	fp->private_data = inode->i_private;
	return 0;
}

#define TMPSIZE 20

static ssize_t dfs_read_file(struct file *fp, char *buf,
		size_t count, loff_t *offset)
{
	atomic_t *counter = (atomic_t *)fp->private_data;
	int v, len;
	char tmp[TMPSIZE];

	/* Encode the value and figure out how much of it we can pass back */
	v = atomic_read(counter);
	if(*offset > 0)
		v -= 1; /* The value returned when offset was zero */
	else
		atomic_inc(counter);

	len = snprintf(tmp, TMPSIZE, "%d\n", v);
	if(*offset > len)
		return 0;

	if(count > len - *offset)
		count = len - *offset;

	/* copy it back, increment the offset, and we are done */
	if(copy_to_user(buf, tmp + *offset, count))
		return -EFAULT;

	*offset += count;

	return count;
}


static ssize_t dfs_write_file(struct file *fp, const char *buf,
		size_t count, loff_t *offset)
{
	atomic_t *counter = (atomic_t *)fp->private_data;
	char tmp[TMPSIZE];

	/* only write from the beginning */
	if(*offset != 0)
		return -EINVAL;

	/* read the value from the user */
	if(count >= TMPSIZE)
		return -EINVAL;

	memset(tmp, 0, TMPSIZE);
	if(copy_from_user(tmp, buf, count))
		return -EFAULT;

	/* store in the counter and we are done */
	atomic_set(counter, simple_strtol(tmp, NULL, 10));

	return count;
}

/* Now we can put together our file operations structure */
static struct file_operations dummyfs_file_ops = {
	.open	= dfs_open,
	.read	= dfs_read_file,
	.write	= dfs_write_file,
};

/* Create a file mapping a name to a counter */
const struct inode_operations dummyfs_inode_ops = {
	.setattr	= simple_setattr,
	.getattr	= simple_getattr,
};


/* @brief create a directory which can be used to hold files. This code is
 * almost identical to the 'create file' logic. except that we create the
 * inode with a different mode, and use the libfs 'simple' operations.
 */
static struct dentry *dummyfs_create_dir(struct super_block *sb,
		struct dentry *parent, const char *name)
{
	struct dentry *dentry;
	struct inode *inode;


	dentry = d_alloc_name(parent, name);
	if(!dentry)
		goto fail;

	inode = dummyfs_make_inode(sb, S_IFDIR | 0755, &simple_dir_operations);
	if(!inode)
		goto fail_dput;

	inode->i_op = &simple_dir_inode_operations;

	d_add(dentry, inode);

	return dentry;

fail_dput:
	dput(dentry);
fail:
	return 0;
}

static struct dentry *dummyfs_create_file(struct super_block *sb,
		struct dentry *dir, const char *name, atomic_t *counter)
{
	struct dentry *dentry;
	struct inode *inode;

	/* Now we can create our dentry and the inode to go with it */
	dentry = d_alloc_name(dir, name);
	if(!dentry)
		goto fail;

	inode = dummyfs_make_inode(sb, S_IFREG | 0644, &dummyfs_file_ops);
	if(!inode)
		goto fail_dput;

	inode->i_private = counter;

	/* Put it all to the dentry cache and we are done */
	d_add(dentry, inode);
	return dentry;

fail_dput:
	dput(dentry);
fail:
	return 0;
}


/* Create the files we can export */
static atomic_t counter, subcounter;

static void dummyfs_create_files(struct super_block *sb, struct dentry *root)
{
	struct dentry *subdir;

	/* one counter in the top-level directory */
	atomic_set(&counter, 0);
	dummyfs_create_file(sb, root, "counter", &counter);

	/* add one in a subdirectory */
	atomic_set(&subcounter, 0);
	subdir = dummyfs_create_dir(sb, root, "subdir");
	if(subdir)
		dummyfs_create_file(sb, subdir, "subcounter", &subcounter);

}


/* @brief superblock operations, both of which are generic kernel operations
 * that we don`t have to write ourselves.
 */
static struct super_operations dummyfs_ops = {
	.statfs		= simple_statfs,
	.drop_inode	= generic_delete_inode,
};

/* @brief fill a superblock with mandatory stuff */
static int dfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode *root;
	struct dentry *root_dentry;

	/* basic parameters */
	sb->s_blocksize = VMACACHE_SIZE;
	sb->s_blocksize_bits = VMACACHE_SIZE;
	sb->s_magic = LFS_MAGIC;
	sb->s_op = &dummyfs_ops;

	/* we need to bring-up an inode to represent the root direcrory
	 * of this file-system. It`s operations all come from libfs, so
	 * we do not have to mess with actually 'doing' things inside this
	 * directory
	 */
	root = dummyfs_make_inode(sb, S_IFDIR | 0755, &simple_dir_operations);

	inode_init_owner(&init_user_ns, root, NULL, S_IFDIR | 0755);

	if(!root)
		goto fail;

	root->i_op = &simple_dir_inode_operations;
	/* root->i_fop = &simple_dir_operations; */

	/* The directory inode must be put into the directory cache (by way
	 * of a "dentry" structre) so that the VFS can find it: get a dentry
	 * to represent the directory in core.
	 */
	set_nlink(root, 2);
	root_dentry = d_make_root(root);
	if(!root_dentry)
		goto fail_iput;

	sb->s_root = root_dentry;

	/* Make up the files which will be in this filesystem, and we`re done */
	dummyfs_create_files(sb, root_dentry);
	sb->s_root = root_dentry;

	return 0;

fail_iput:
	iput(root);
fail:
	return -ENOMEM;
}

/* @brief Stuff to pass-in when registering the filesystem */
static struct dentry *dummyfs_mount_superblock(struct file_system_type *fst,
		int flags, const char *devname, void *data)
{
	pr_info("dummyfs: mounting filesystem...\n");
	return mount_nodev(fst, flags, data, dfs_fill_super);
}

static struct file_system_type dummyfs_type = {
	.owner		= THIS_MODULE,
	.name		= "dummyfs",
	.mount		= dummyfs_mount_superblock,	/* < v3 .get_sb */
	.kill_sb	= kill_litter_super,		/* generic kernel func */
};


/* @brief Setup for registration/unregistration virtual filesystem */
static int __init dummyfs_init(void)
{
	pr_info("registering dummy file-system");

	/* int register_filesystem(struct file_system_type * fs) */
	if(register_filesystem(&dummyfs_type) < 0){
		pr_err("dummyfs: ERROR, can`t register fs\n");
		return -1;
	}

	pr_info("dummyfs::registration success\n");

	return 0;
}

static void __exit dummyfs_fini(void)
{
	pr_info("unregistering dummy file-system");

	if(unregister_filesystem(&dummyfs_type) < 0)
		pr_err("dummyfs: ERROR, can`t unregister fs!\n");

	pr_info("dummyfs::deletion success\n");

	return;
}

module_init(dummyfs_init);
module_exit(dummyfs_fini);

/* mount -t dummyfs none /dir/ */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Oleksii Nedopytalskyi");
MODULE_DESCRIPTION("creating own linux virtual-fs,\
		referenced: Jonathan Corbet https://lwn.net/Articles/57369/");
