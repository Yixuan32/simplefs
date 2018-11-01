/*
 * A Simple Filesystem for the Linux Kernel.
 *
 * Initial author: Sankar P <sankar.curiosity@gmail.com>
 * License: Creative Commons Zero License - http://creativecommons.org/publicdomain/zero/1.0/
 *
 * TODO: we need to split it into smaller files
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/version.h>
#include <linux/time64.h>

#include "super.h"

#define f_dentry f_path.dentry
/* A super block lock that must be used for any critical section operation on the sb,
 * such as: updating the free_blocks, inodes_count etc. */
static DEFINE_MUTEX(simplefs_sb_lock);
static DEFINE_MUTEX(simplefs_inodes_mgmt_lock);

/* FIXME: This can be moved to an in-memory structure of the simplefs_inode.
 * Because of the global nature of this lock, we cannot create
 * new children (without locking) in two different dirs at a time.
 * They will get sequentially created. If we move the lock
 * to a directory-specific way (by moving it inside inode), the
 * insertion of two children in two different directories can be
 * done in parallel */
static DEFINE_MUTEX(simplefs_directory_children_update_lock);

static struct kmem_cache *sfs_inode_cachep;
//同步超级块
void simplefs_sb_sync(struct super_block *vsb)
{
	struct buffer_head *bh;
	struct simplefs_super_block *sb = SIMPLEFS_SB(vsb);
	
	bh = sb_bread(vsb, SIMPLEFS_SUPERBLOCK_BLOCK_NUMBER);
	BUG_ON(!bh);

	bh->b_data = (char *)sb;
	/* 标记缓冲区首部为脏 */
	mark_buffer_dirty(bh);
	/* 然后同步 */
	sync_dirty_buffer(bh);
	/*释放bh指针*/
	brelse(bh);
}

struct simplefs_inode *simplefs_inode_search(struct super_block *sb,
		struct simplefs_inode *start,
		struct simplefs_inode *search)
{
	uint64_t count = 0;
	while (start->inode_no != search->inode_no
			&& count < SIMPLEFS_SB(sb)->inodes_count) {
		count++;
		start++;
	}

	if (start->inode_no == search->inode_no) {
		return start;
	}

	return NULL;
}

void simplefs_inode_add(struct super_block *vsb, struct simplefs_inode *inode)
{
	struct simplefs_super_block *sb = SIMPLEFS_SB(vsb);
	struct buffer_head *bh;
	struct simplefs_inode *inode_iterator;

	if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
		sfs_trace("Failed to acquire mutex lock\n");
		return;
	}

	//存放Inode信息的数据区
	bh = sb_bread(vsb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
	BUG_ON(!bh);
	//因为这个数据区依次存放的都是simplefs_inode的结构，因此做下强制转换
	inode_iterator = (struct simplefs_inode *)bh->b_data;

	if (mutex_lock_interruptible(&simplefs_sb_lock)) {
		sfs_trace("Failed to acquire mutex lock\n");
		return;
	}

	/* Append the new inode in the end in the inode store */
	/*先将inode_iterator指向空闲的Inode区域*/
	inode_iterator += sb->inodes_count;
	//拷贝Inode信息到对应的位置
	memcpy(inode_iterator, inode, sizeof(struct simplefs_inode));
	//将超级块中的Inode计数自增
	sb->inodes_count++;

	//先将当前的数据块标记为脏，等待回写磁盘
	mark_buffer_dirty(bh);
	//同理超级块也需要更新
	simplefs_sb_sync(vsb);
	/*释放Inode的数据块*/
	brelse(bh);

	mutex_unlock(&simplefs_sb_lock);
	mutex_unlock(&simplefs_inodes_mgmt_lock);
}

/* This function returns a blocknumber which is free.
 * The block will be removed from the freeblock list.
 *
 * In an ideal, production-ready filesystem, we will not be dealing with blocks,
 * and instead we will be using extents
 *
 * If for some reason, the file creation/deletion failed, the block number
 * will still be marked as non-free. You need fsck to fix this.*/
// sb->free_blocks对应的Bit位如果为1，那么说明对应的数据块空闲，否则该数据块Busy
int simplefs_sb_get_a_freeblock(struct super_block *vsb, uint64_t * out)
{
	//通过内核标准的SuperBlock结构获取特定文件系统的SB结构
	struct simplefs_super_block *sb = SIMPLEFS_SB(vsb);
	int i;
	int ret = 0;

	if (mutex_lock_interruptible(&simplefs_sb_lock)) {
		sfs_trace("Failed to acquire mutex lock\n");
		ret = -EINTR;
		goto end;
	}

	//需要注意的是，数据块是从第3个开始的，原因是根节点，超级块，以及块设备中存放的默认文件，本身已经在文件系统了
	// sb->free_blocks中的每一个Bit代表了一个数据块
	/* Loop until we find a free block. We start the loop from 3,
	 * as all prior blocks will always be in use */
	for (i = 3; i < SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED; i++) {
		//如果sb->free_blocks的对应Bit为0，则意味着当前数据块是空闲的可以使用
		if (sb->free_blocks & (1 << i)) {
			break;
		}
	}

	//如果上面的循环从3~SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED,找了一圈还是没有找到空闲的数据块
	//则说明该文件系统没有剩余的空间了，返回出错信息
	if (unlikely(i == SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED)) {
		printk(KERN_ERR "No more free blocks available");
		ret = -ENOSPC;
		goto end;
	}

	//如果找到空闲的数据块，则返回该数据块的索引
	*out = i;

	//既然找到了空闲的数据块，那么需要将sb->free_blocks的对应Bit置位
	/* Remove the identified block from the free list */
	sb->free_blocks &= ~(1 << i);

	//另外我们还需要将对应的数据块标记为dirty，并回写到磁盘
	simplefs_sb_sync(vsb);

end:
	mutex_unlock(&simplefs_sb_lock);
	return ret;
}

static int simplefs_sb_get_objects_count(struct super_block *vsb,
					 uint64_t * out)
{
	struct simplefs_super_block *sb = SIMPLEFS_SB(vsb);

	if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
		sfs_trace("Failed to acquire mutex lock\n");
		return -EINTR;
	}
	*out = sb->inodes_count;
	mutex_unlock(&simplefs_inodes_mgmt_lock);

	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
/*这个函数在"ls"指令的时候会被调度到*/
static int simplefs_iterate(struct file *filp, struct dir_context *ctx)
#else
static int simplefs_readdir(struct file *filp, void *dirent, filldir_t filldir)
#endif
{
	loff_t pos;
	struct inode *inode;
	struct super_block *sb;
	struct buffer_head *bh;
	struct simplefs_inode *sfs_inode;
	struct simplefs_dir_record *record;
	int i;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
	pos = ctx->pos;
#else
	pos = filp->f_pos;
#endif
	//通过文件从而获取到对应的Inode指针
	inode = filp->f_dentry->d_inode;
	//进一步的通过Inode获取到文件系统的SuperBlock
	sb = inode->i_sb;
	printk("%s LINE = %d\n",__func__,__LINE__);
	if (pos) {
		/* FIXME: We use a hack of reading pos to figure if we have filled in all data.
		 * We should probably fix this to work in a cursor based model and
		 * use the tokens correctly to not fill too many data in each cursor based call */
		return 0;
	}

	//通过内核标准的Inode从而获取到特定文件系统的Inode结构
	sfs_inode = SIMPLEFS_INODE(inode);

	//这个函数只会在目录下才能操作，因此不满足此条件则是非法
	if (unlikely(!S_ISDIR(sfs_inode->mode))) {
		printk(KERN_ERR
		       "inode [%llu][%lu] for fs object [%s] not a directory\n",
		       sfs_inode->inode_no, inode->i_ino,
		       filp->f_dentry->d_name.name);
		return -ENOTDIR;
	}

	//获取该目录下的内容指针
	bh = sb_bread(sb, sfs_inode->data_block_number);
	BUG_ON(!bh);
	//目录下存放的内容都是simplefs_dir_record结构，强制转换一下
	record = (struct simplefs_dir_record *)bh->b_data;
	for (i = 0; i < sfs_inode->dir_children_count; i++) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
		//逐一上报当前目录下的文件名
		dir_emit(ctx, record->filename, SIMPLEFS_FILENAME_MAXLEN,
			record->inode_no, DT_UNKNOWN);
		ctx->pos += sizeof(struct simplefs_dir_record);
#else
		filldir(dirent, record->filename, SIMPLEFS_FILENAME_MAXLEN, pos,
			record->inode_no, DT_UNKNOWN);
		filp->f_pos += sizeof(struct simplefs_dir_record);
#endif
		pos += sizeof(struct simplefs_dir_record);
		record++;
	}
	//释放数据块的指针
	brelse(bh);

	return 0;
}

/* This functions returns a simplefs_inode with the given inode_no
 * from the inode store, if it exists. */
//从Inode的起始域开始根据Inode号查询到对应的Inode信息，并返回。
struct simplefs_inode *simplefs_get_inode(struct super_block *sb,
					  uint64_t inode_no)
{
	struct simplefs_super_block *sfs_sb = SIMPLEFS_SB(sb);
	struct simplefs_inode *sfs_inode = NULL;
	struct simplefs_inode *inode_buffer = NULL;

	int i;
	struct buffer_head *bh;

	/* The inode store can be read once and kept in memory permanently while mounting.
	 * But such a model will not be scalable in a filesystem with
	 * millions or billions of files (inodes) */
	//指向Inode的起始域
	bh = sb_bread(sb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
	BUG_ON(!bh);
	//将其强制转换为simplefs_inode类型的指针
	sfs_inode = (struct simplefs_inode *)bh->b_data;

#if 0
	if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
		printk(KERN_ERR "Failed to acquire mutex lock %s +%d\n",
		       __FILE__, __LINE__);
		return NULL;
	}
#endif
	//根据指定的Inode索引获取Inode的相关信息，并返回
	for (i = 0; i < sfs_sb->inodes_count; i++) {
		if (sfs_inode->inode_no == inode_no) {
			inode_buffer = kmem_cache_alloc(sfs_inode_cachep, GFP_KERNEL);
			memcpy(inode_buffer, sfs_inode, sizeof(*inode_buffer));

			break;
		}
		sfs_inode++;
	}
//      mutex_unlock(&simplefs_inodes_mgmt_lock);

	brelse(bh);
	return inode_buffer;
}

ssize_t simplefs_read(struct file * filp, char __user * buf, size_t len,
		      loff_t * ppos)
{
	/* After the commit dd37978c5 in the upstream linux kernel,
	 * we can use just filp->f_inode instead of the
	 * f->f_path.dentry->d_inode redirection */
	struct simplefs_inode *inode =
	    SIMPLEFS_INODE(filp->f_path.dentry->d_inode);
	struct buffer_head *bh;

	char *buffer;
	int nbytes;

	//如果要读数据的偏移超出了该Inode的大小，那么直接返回读取长度为0
	if (*ppos >= inode->file_size) {
		/* Read request with offset beyond the filesize */
		return 0;
	}

	//得到该Inode的数据区，将其读取出来
	bh = sb_bread(filp->f_path.dentry->d_inode->i_sb,
					    inode->data_block_number);

	if (!bh) {
		printk(KERN_ERR "Reading the block number [%llu] failed.",
		       inode->data_block_number);
		return 0;
	}
	//将数据区强制转换为Char*
	buffer = (char *)bh->b_data;
	//既然是读，就要考虑到有可能你读取的长度会超过该Inode的大小，因此需要取俩者中的最大值
	nbytes = min((size_t) inode->file_size, len);
	//该Inode读取到的数据传递给用户层
	if (copy_to_user(buf, buffer, nbytes)) {
		brelse(bh);
		printk(KERN_ERR
		       "Error copying file contents to the userspace buffer\n");
		return -EFAULT;
	}
	//由于读取操作不会改变磁盘的内容因此不需要同步操作，直接释放数据块的指针
	brelse(bh);
	//改变游标的指针
	*ppos += nbytes;
	//返回读取的长度
	return nbytes;
}

/* Save the modified inode */
int simplefs_inode_save(struct super_block *sb, struct simplefs_inode *sfs_inode)
{
	struct simplefs_inode *inode_iterator;
	struct buffer_head *bh;
	//先读取Inode的数据区
	bh = sb_bread(sb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
	BUG_ON(!bh);

	if (mutex_lock_interruptible(&simplefs_sb_lock)) {
		sfs_trace("Failed to acquire mutex lock\n");
		return -EINTR;
	}
	//从Inode的数据区中匹配要更新的Inode
	inode_iterator = simplefs_inode_search(sb,
		(struct simplefs_inode *)bh->b_data,
		sfs_inode);

	if (likely(inode_iterator)) {
		/*更新Inode*/
		memcpy(inode_iterator, sfs_inode, sizeof(*inode_iterator));
		printk(KERN_INFO "The inode updated\n");
		//将Inode的数据区设置为Dirty，并同步
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
	} else {
		mutex_unlock(&simplefs_sb_lock);
		printk(KERN_ERR
		       "The new filesize could not be stored to the inode.");
		return -EIO;
	}
	//释放该数据区
	brelse(bh);

	mutex_unlock(&simplefs_sb_lock);

	return 0;
}

/* FIXME: The write support is rudimentary. I have not figured out a way to do writes
 * from particular offsets (even though I have written some untested code for this below) efficiently. */
ssize_t simplefs_write(struct file * filp, const char __user * buf, size_t len,
		       loff_t * ppos)
{
	/* After the commit dd37978c5 in the upstream linux kernel,
	 * we can use just filp->f_inode instead of the
	 * f->f_path.dentry->d_inode redirection */
	struct inode *inode;
	struct simplefs_inode *sfs_inode;
	struct buffer_head *bh;
	struct super_block *sb;

	char *buffer;

	int retval;

#if 0
	retval = generic_write_checks(filp, ppos, &len, 0);
	if (retval) {
		return retval;
	}
#endif
	//通过文件得到内核对应的Inode指针
	inode = filp->f_path.dentry->d_inode;
	//通过内核的Inode指针进而得到特定文件系统的Inode指针
	sfs_inode = SIMPLEFS_INODE(inode);
	//通过Inode得到SuperBlock
	sb = inode->i_sb;
	//获取该Inode指向的数据块
	bh = sb_bread(filp->f_path.dentry->d_inode->i_sb,
					    sfs_inode->data_block_number);

	if (!bh) {
		printk(KERN_ERR "Reading the block number [%llu] failed.",
		       sfs_inode->data_block_number);
		return 0;
	}
	
	//将数据区强制转换为char*型
	buffer = (char *)bh->b_data;

	/* Move the pointer until the required byte offset */
	//移动到vfs指定的偏移位置
	buffer += *ppos;

	//拷贝用户空间的数据到对应的数据块中
	if (copy_from_user(buffer, buf, len)) {
		brelse(bh);
		printk(KERN_ERR
		       "Error copying file contents from the userspace buffer to the kernel space\n");
		return -EFAULT;
	}
	//通知VFS指针偏移了多少
	*ppos += len;
	//将数据区设置为Dirty，并回写到磁盘
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	//最后释放数据块的指针
	brelse(bh);

	/* Set new size
	 * sfs_inode->file_size = max(sfs_inode->file_size, *ppos);
	 *
	 * FIXME: What to do if someone writes only some parts in between ?
	 * The above code will also fail in case a file is overwritten with
	 * a shorter buffer */
	if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
		sfs_trace("Failed to acquire mutex lock\n");
		return -EINTR;
	}
	//更新Inode的文件大小
	sfs_inode->file_size = *ppos;
	//既然更新了Inode的信息，那么Inode的信息区也要同步更新下
	retval = simplefs_inode_save(sb, sfs_inode);
	if (retval) {
		len = retval;
	}
	mutex_unlock(&simplefs_inodes_mgmt_lock);

	return len;
}

const struct file_operations simplefs_file_operations = {
	.read = simplefs_read,
	.write = simplefs_write,
};

const struct file_operations simplefs_dir_operations = {
	.owner = THIS_MODULE,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
	.iterate = simplefs_iterate,
#else
	.readdir = simplefs_readdir,
#endif
};

struct dentry *simplefs_lookup(struct inode *parent_inode,
			       struct dentry *child_dentry, unsigned int flags);

static int simplefs_create(struct inode *dir, struct dentry *dentry,
			   umode_t mode, bool excl);

static int simplefs_mkdir(struct inode *dir, struct dentry *dentry,
			  umode_t mode);

static struct inode_operations simplefs_inode_ops = {
	.create = simplefs_create,
	.lookup = simplefs_lookup,
	.mkdir = simplefs_mkdir,
};

static int simplefs_create_fs_object(struct inode *dir, struct dentry *dentry,
				     umode_t mode)
{
	struct inode *inode;
	struct simplefs_inode *sfs_inode;
	struct super_block *sb;
	struct simplefs_inode *parent_dir_inode;
	struct buffer_head *bh;
	struct simplefs_dir_record *dir_contents_datablock;
	uint64_t count;
	int ret;

	if (mutex_lock_interruptible(&simplefs_directory_children_update_lock)) {
		sfs_trace("Failed to acquire mutex lock\n");
		return -EINTR;
	}
	//通过这个父Inode获取到这个文件系统的SuperBlock
	sb = dir->i_sb;
	
	//我们首先思考，我们如果想要创建一个Inode是不是应该看下该文件系统的Inode位置是否		
	//还有空余来允许我们创建呢，因此，我们先要得到当前文件系统已经使用的Inode总数。
	ret = simplefs_sb_get_objects_count(sb, &count);
	if (ret < 0) {
		mutex_unlock(&simplefs_directory_children_update_lock);
		return ret;
	}

	//先判断Inode总数是否超了，如果是，则返回用户没有空间创建了
	if (unlikely(count >= SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED)) {
		/* The above condition can be just == insted of the >= */
		printk(KERN_ERR
		       "Maximum number of objects supported by simplefs is already reached");
		mutex_unlock(&simplefs_directory_children_update_lock);
		return -ENOSPC;
	}

	//该文件系统只支持目录和普通文件的创建，否则返回出错
	if (!S_ISDIR(mode) && !S_ISREG(mode)) {
		printk(KERN_ERR
		       "Creation request but for neither a file nor a directory");
		mutex_unlock(&simplefs_directory_children_update_lock);
		return -EINVAL;
	}
	
	//通过SuperBlock创建一个空的Inode  
	inode = new_inode(sb);
	if (!inode) {
		mutex_unlock(&simplefs_directory_children_update_lock);
		return -ENOMEM;
	}
	//设置这个Inode指向的SuperBlock   
	inode->i_sb = sb;
	//设置这个Inode的操作指针
	inode->i_op = &simplefs_inode_ops;
	//设置这个Inode的创建时间
	inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
	//设置Inode的Inode号
	inode->i_ino = (count + SIMPLEFS_START_INO - SIMPLEFS_RESERVED_INODES + 1);
	//创建特定文件系统的Inode结构
	sfs_inode = kmem_cache_alloc(sfs_inode_cachep, GFP_KERNEL);
	//对该节点的Inode号赋值
	sfs_inode->inode_no = inode->i_ino;
	//将内核标准节点的私有指针指向当前特定文件系统的Inode结构
	inode->i_private = sfs_inode;
	//设置文件系统的属性
	sfs_inode->mode = mode;

	//对文件目录以及普通文件分别做设置，需要注意的是，如果创建的是一个目录，那么毫无疑问，当前目录下
	//的Inode个数肯定还是为0的
	if (S_ISDIR(mode)) {
		printk(KERN_INFO "New directory creation request\n");
		sfs_inode->dir_children_count = 0;
		inode->i_fop = &simplefs_dir_operations;
	} else if (S_ISREG(mode)) {
		printk(KERN_INFO "New file creation request\n");
		sfs_inode->file_size = 0;
		//针对普通文件设置读写操作
		inode->i_fop = &simplefs_file_operations;
	}

	/* First get a free block and update the free map,
	 * Then add inode to the inode store and update the sb inodes_count,
	 * Then update the parent directory's inode with the new child.
	 *
	 * The above ordering helps us to maintain fs consistency
	 * even in most crashes
	 */
	//从超级块中获取空闲的数据块
	ret = simplefs_sb_get_a_freeblock(sb, &sfs_inode->data_block_number);
	if (ret < 0) {
		printk(KERN_ERR "simplefs could not get a freeblock");
		mutex_unlock(&simplefs_directory_children_update_lock);
		return ret;
	}
	//新建一个Inode需要更新Inode的数据区，并同步
	simplefs_inode_add(sb, sfs_inode);

	/*除了更新Inode的数据区，我们还需要做一件事:在父目录(Inode)下面，添加该Inode的信息*/
	//既然要添加信息，必须首先取得当前父目录的结构信息，通过内核的标准Inode获取特定文件系统的Inode
	//信息
	parent_dir_inode = SIMPLEFS_INODE(dir);
	//通过simplefs_inode中的成员从而获取到数据信息
	bh = sb_bread(sb, parent_dir_inode->data_block_number);
	BUG_ON(!bh);
	//需要知道的是目录Inode中存放的内容结构都是固定的，因此做下强制转换
	dir_contents_datablock = (struct simplefs_dir_record *)bh->b_data;

	/* Navigate to the last record in the directory contents */
	/*让DIR的内容指针指向空闲区域*/
	dir_contents_datablock += parent_dir_inode->dir_children_count;
	/*让DIR的内容inode_no以及文件名称更新到父目录中*/
	dir_contents_datablock->inode_no = sfs_inode->inode_no;
	strcpy(dir_contents_datablock->filename, dentry->d_name.name);

	/*将父目录指向的数据块设置为dirty，并将其回写到磁盘，之后释放*/
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
		mutex_unlock(&simplefs_directory_children_update_lock);
		sfs_trace("Failed to acquire mutex lock\n");
		return -EINTR;
	}
	//将父目录中的dir_children_count也自增
	parent_dir_inode->dir_children_count++;
	//同理我们更改了Inode数据区，这个数据区自然也要同步下了
	ret = simplefs_inode_save(sb, parent_dir_inode);
	if (ret) {
		mutex_unlock(&simplefs_inodes_mgmt_lock);
		mutex_unlock(&simplefs_directory_children_update_lock);

		/* TODO: Remove the newly created inode from the disk and in-memory inode store
		 * and also update the superblock, freemaps etc. to reflect the same.
		 * Basically, Undo all actions done during this create call */
		return ret;
	}

	mutex_unlock(&simplefs_inodes_mgmt_lock);
	mutex_unlock(&simplefs_directory_children_update_lock);
	//将当前Inode和其父目录关联
	inode_init_owner(inode, dir, mode);
	//将当前文件加入到denrty下面
	d_add(dentry, inode);

	return 0;
}

static int simplefs_mkdir(struct inode *dir, struct dentry *dentry,
			  umode_t mode)
{
	/* I believe this is a bug in the kernel, for some reason, the mkdir callback
	 * does not get the S_IFDIR flag set. Even ext2 sets is explicitly */
	 
	printk("%s LINE = %d\n",__func__,__LINE__);
	return simplefs_create_fs_object(dir, dentry, S_IFDIR | mode);
}

static int simplefs_create(struct inode *dir, struct dentry *dentry,
			   umode_t mode, bool excl)
{
	printk("%s LINE = %d\n",__func__,__LINE__);

	return simplefs_create_fs_object(dir, dentry, mode);
}

struct dentry *simplefs_lookup(struct inode *parent_inode,
			       struct dentry *child_dentry, unsigned int flags)
{
	//从根节点的Inode获取在Mount的时候读取到的磁盘中Sb的信息
	struct simplefs_inode *parent = SIMPLEFS_INODE(parent_inode);
	struct super_block *sb = parent_inode->i_sb;
	struct buffer_head *bh;
	struct simplefs_dir_record *record;
	int i;
	//因为根节点中会包含Data Block的索引，通过这个索引获取根节点这个dentry中存放的内容
	/*      根节点(Dentry)的内容如下:
	 *      文件名:  "vanakkam"     
	 *      Inode号:  2
	 */
	bh = sb_bread(sb, parent->data_block_number);
	BUG_ON(!bh);

	//指向父目录Inode中内容头部
	record = (struct simplefs_dir_record *)bh->b_data;
	//先确定该目录下面有多少个Inode(这里是包含文件和目录的)
	for (i = 0; i < parent->dir_children_count; i++) {
		//如果遍历了整个目录后，都无法找到这个文件或者文件夹，那么直接返回NULL，让VFS为其新
		//建一个Inode,child_dentry->d_name.name是希望创建的目标文件或者文件夹  
		if (!strcmp(record->filename, child_dentry->d_name.name)) {
			/* FIXME: There is a corner case where if an allocated inode,
			 * is not written to the inode store, but the inodes_count is
			 * incremented. Then if the random string on the disk matches
			 * with the filename that we are comparing above, then we
			 * will use an invalid uninitialized inode */
			/*
			 * 进入这个分支是一种比较极端的情况，意味着分配的Inode并没有被写入存储区，但是InodesCount确递增了
			 * 同时磁盘上的Inode的随机字符串和我们比较的字符串匹配上了，那么我们直接使用这个非法的未初始化的
			 * Inode即可
			 */
			struct inode *inode;
			struct simplefs_inode *sfs_inode;

			//使用这个未被写入存储区的Inode号，查询到具体的Inode信息结构
			sfs_inode = simplefs_get_inode(sb, record->inode_no);
			//使用SuperBlock分配一个空闲的Inode
			inode = new_inode(sb);
			//设置Inode号
			inode->i_ino = record->inode_no;
			//设置这个Inode归属于哪个父节点(Inode)下，同时设置其模式(例如：表明其是目录还是文件)
			inode_init_owner(inode, parent_inode, sfs_inode->mode);
			//该Inode指向的超级块指针需要设置
			inode->i_sb = sb;
			//设置该Inode的节点操作指针(因为有可能这个节点它是一个目录，那就需要支持mkdir,touch等操作)
			inode->i_op = &simplefs_inode_ops;

			//针对目录和常规文件的操作指针
			if (S_ISDIR(inode->i_mode))
				inode->i_fop = &simplefs_dir_operations;
			else if (S_ISREG(inode->i_mode))
				inode->i_fop = &simplefs_file_operations;
			else
				printk(KERN_ERR
				       "Unknown inode type. Neither a directory nor a file");

			/* FIXME: We should store these times to disk and retrieve them */
			//设置Inode的创建时间
			inode->i_atime = inode->i_mtime = inode->i_ctime =
			    CURRENT_TIME;

			//如前所述，i_private指针指向的是Inode具体信息(文件大小,属性,数据块的位置)
			inode->i_private = sfs_inode;

			//将该Inode加入到当前目录(Dentry)下
			d_add(child_dentry, inode);
			//由于我们使用的是之前分配的inode号(未写入存储区)，因此并不需要再重新创建了，直接
			//返回NULL即可
			return NULL;
		}
		record++;
	}

	printk(KERN_ERR
	       "No inode found for the filename [%s]\n",
	       child_dentry->d_name.name);

	return NULL;
}


/**
 * Simplest
 */
void simplefs_destory_inode(struct inode *inode)
{
	struct simplefs_inode *sfs_inode = SIMPLEFS_INODE(inode);

	printk(KERN_INFO "Freeing private data of inode %p (%lu)\n",
	       sfs_inode, inode->i_ino);
	kmem_cache_free(sfs_inode_cachep, sfs_inode);
}

static const struct super_operations simplefs_sops = {
	.destroy_inode = simplefs_destory_inode,
};

/* This function, as the name implies, Makes the super_block valid and
 * fills filesystem specific information in the super block */
int simplefs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode *root_inode;
	struct buffer_head *bh;
	struct simplefs_super_block *sb_disk;
	int ret = -EPERM;

	bh = sb_bread(sb, SIMPLEFS_SUPERBLOCK_BLOCK_NUMBER);
	BUG_ON(!bh);
	//获取磁盘中存放的super block的真实内容
	sb_disk = (struct simplefs_super_block *)bh->b_data;

	printk(KERN_INFO "The magic number obtained in disk is: [%llu]\n",
	       sb_disk->magic);

	if (unlikely(sb_disk->magic != SIMPLEFS_MAGIC)) {
		printk(KERN_ERR
		       "The filesystem that you try to mount is not of type simplefs. Magicnumber mismatch.");
		goto release;
	}

	if (unlikely(sb_disk->block_size != SIMPLEFS_DEFAULT_BLOCK_SIZE)) {
		printk(KERN_ERR
		       "simplefs seem to be formatted using a non-standard block size.");
		goto release;
	}

	printk(KERN_INFO
	       "simplefs filesystem of version [%llu] formatted with a block size of [%llu] detected in the device.\n",
	       sb_disk->version, sb_disk->block_size);

	/* A magic number that uniquely identifies our filesystem type */
	//sb中的魔数和磁盘中的一致
	sb->s_magic = SIMPLEFS_MAGIC;

	/* For all practical purposes, we will be using this s_fs_info as the super block */
	//指向文件系统信息的数据结构，
	sb->s_fs_info = sb_disk;
	//表明当前文件系统最大数据块为4K
	sb->s_maxbytes = SIMPLEFS_DEFAULT_BLOCK_SIZE;
	//实现超级块的指针,不是必须实现的
	sb->s_op = &simplefs_sops;

	//为我们的根节点分配一个Inode
	root_inode = new_inode(sb);
	//设置根节点的Inode编号
	root_inode->i_ino = SIMPLEFS_ROOTDIR_INODE_NUMBER;
	//声明这个Inode是一个目录，因为是根dentry所以它的所在目录为NULL
	inode_init_owner(root_inode, NULL, S_IFDIR);
	//指向所在文件系统的超级块
	root_inode->i_sb = sb;
	//因为需要在根节点下进行操作，因此需要实现节点的操作指针
	/*
	 * 	1.创建一个普通文件，调用create指针;
	 * 	2.创建一个普通文件之前，还需要先调用lookup指针;
	 * 	3.创建一个目录，调用mkdir;
	 */
	root_inode->i_op = &simplefs_inode_ops;
	//Provide Io Operation For UserSpace,eg:readdir
	//是否需要支持文件的操作，比如读写,mmap等
	root_inode->i_fop = &simplefs_dir_operations;
	//Inode的创建时间戳
	root_inode->i_atime = root_inode->i_mtime = root_inode->i_ctime =
	    CURRENT_TIME;
	//既然是根节点，自然是需要获取根节点的内容，这里的i_private就是指向根节点信息的
	/*
		信息如下:
		1.根节点号
		2.根节点中数据内容存放的位置-> DATA_BLOCK_BUMBER
		3.根节点下面的子节点个数
	*/
	root_inode->i_private =
	    simplefs_get_inode(sb, SIMPLEFS_ROOTDIR_INODE_NUMBER);

	/* TODO: move such stuff into separate header. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 3, 0)
	//Super Block需要告知当前文件系统的根dentry，而这个dentry本质上也是来自于一个inode
	sb->s_root = d_make_root(root_inode);
#else
	sb->s_root = d_alloc_root(root_inode);
	if (!sb->s_root)
		iput(root_inode);
#endif

	if (!sb->s_root) {
		ret = -ENOMEM;
		goto release;
	}

	ret = 0;
release:
	brelse(bh);

	return ret;
}

static struct dentry *simplefs_mount(struct file_system_type *fs_type,
				     int flags, const char *dev_name,
				     void *data)
{
	struct dentry *ret;

	ret = mount_bdev(fs_type, flags, dev_name, data, simplefs_fill_super);

	if (unlikely(IS_ERR(ret)))
		printk(KERN_ERR "Error mounting simplefs");
	else
		printk(KERN_INFO "simplefs is succesfully mounted on [%s]\n",
		       dev_name);

	return ret;
}

static void simplefs_kill_superblock(struct super_block *sb)
{
	printk(KERN_INFO
	       "simplefs superblock is destroyed. Unmount succesful.\n");
	/* This is just a dummy function as of now. As our filesystem gets matured,
	 * we will do more meaningful operations here */

	kill_block_super(sb);
	return;
}

struct file_system_type simplefs_fs_type = {
	.owner = THIS_MODULE,
	.name = "simplefs",
	.mount = simplefs_mount,
	.kill_sb = simplefs_kill_superblock,
	.fs_flags = FS_REQUIRES_DEV,
};

static int simplefs_init(void)
{
	int ret;

	printk("%s LINE = %d\n",__func__,__LINE__);
	sfs_inode_cachep = kmem_cache_create("sfs_inode_cache",
	                                     sizeof(struct simplefs_inode),
	                                     0,
	                                     (SLAB_RECLAIM_ACCOUNT| SLAB_MEM_SPREAD),
	                                     NULL);
	if (!sfs_inode_cachep) {
		return -ENOMEM;
	}

	ret = register_filesystem(&simplefs_fs_type);
	if (likely(ret == 0))
		printk(KERN_INFO "Sucessfully registered simplefs\n");
	else
		printk(KERN_ERR "Failed to register simplefs. Error:[%d]", ret);

	return ret;
}

static void simplefs_exit(void)
{
	int ret;

	ret = unregister_filesystem(&simplefs_fs_type);
	kmem_cache_destroy(sfs_inode_cachep);

	if (likely(ret == 0))
		printk(KERN_INFO "Sucessfully unregistered simplefs\n");
	else
		printk(KERN_ERR "Failed to unregister simplefs. Error:[%d]",
		       ret);
}

module_init(simplefs_init);
module_exit(simplefs_exit);

MODULE_LICENSE("CC0");
MODULE_AUTHOR("Sankar P");
