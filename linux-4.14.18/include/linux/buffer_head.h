/* SPDX-License-Identifier: GPL-2.0 */
/*
 * include/linux/buffer_head.h
 *
 * Everything to do with buffer_heads.
 */

#ifndef _LINUX_BUFFER_HEAD_H
#define _LINUX_BUFFER_HEAD_H

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/linkage.h>
#include <linux/pagemap.h>
#include <linux/wait.h>
#include <linux/atomic.h>

#ifdef CONFIG_BLOCK

enum bh_state_bits {
    /*缓冲区数据是可用的:从磁盘读数据到buffer或将dirty数据写入磁盘*/
	BH_Uptodate,	/* Contains valid data */
    /*缓冲区数据新于磁盘数据:如写文件*/
	BH_Dirty,	/* Is dirty */
    /*buffer的数据正在进行操作(磁盘IO,memset),被锁定,防止并发操作*/
	BH_Lock,	/* Is locked */
    /*已提交IO.什么时候清除？多数标记BH_New时清除(因为刚创建的块不可能已提交IO),但这又有什么用呢？*/
	BH_Req,		/* Has been submitted for I/O */
    /*目前在Async_Read/Async_Write中使用,序列化的检测page中的buffer完成状态*/
	BH_Uptodate_Lock,/* Used by the first bh in a page, to serialise
			  * IO completion of other buffers in the page
			  */
    /*缓冲区已绑定块设备的具体块即bdev与b_blocknr赋值*/
	BH_Mapped,	/* Has a disk mapping */
    /*buffer关联的block是由文件系统新分配的:例如在文件尾部追加数据且数据超过原EOF的block.*/
	BH_New,		/* Disk mapping was newly created by get_block */
    /*Async_Read/Async_Write 以页为单位发起读/写,page中的buffers读/写是异步的,
    此标志可以告知end_buffer_async_xxx page中的所有buffer是否已经读/写完成,从而unlock page. 
    若没有此标志,单纯的BH_lock标志不能区分此情况:因为读/写完成的的buffer可能再次被其次进程lock
    */ 
	BH_Async_Read,	/* Is under end_buffer_async_read I/O */
	BH_Async_Write,	/* Is under end_buffer_async_write I/O */
    /*缓冲区指定了块设备,但未指定块号.有以两种情况: 
      1.文件系统已有分配块号对应,但未与之关联.
      2.文件系统未分配块号对应.
      Delay指的是第二种情况,对于文件空洞与追加内容超过EOF块,此时新增的内容可跨越多个块,
      可先保存文件数据延后分配磁盘物理块,保证文件在磁盘中的连续性,可提高文件的读写性能.
      (延迟分配:在进程写数据时不立即分配块,当文件保持在cache中时他会延迟分配块,
      直到数据真的写入磁盘时才分配,这样就给了时间让块分配器优化当前的情况)
    */
	BH_Delay,	/* Buffer is not yet allocated on disk */
    /*
    物理block寻址分为直接寻址与间接寻址(相当于指针), 
    间接寻址需要先读取映射块(逻辑block与物理block映射描述)才能进行物理block寻址. 
    Boundary表示此块可能接近需要进行IO请求的映射块,需提交之前积累的IO操作.如:
    例子参见mpage_readpages解释.
    */
	BH_Boundary,	/* Block is followed by a discontiguity */
	BH_Write_EIO,	/* I/O error on write */
    /*
    预先分配物理块给文件,但并未写入数据,以保证文件数据在磁盘中的连续性, 
    防碎片化:ext4 or 其它文件的特性 (p2p下载是否此场景之一呢???)*/
	BH_Unwritten,	/* Buffer is allocated on disk but not written */
	BH_Quiet,	/* Buffer Error Prinks to be quiet */
    /*包含元数据,在哪设置的呢???*/
	BH_Meta,	/* Buffer contains metadata */
	BH_Prio,	/* Buffer should be submitted with REQ_PRIO */
	BH_Defer_Completion, /* Defer AIO completion to workqueue */

	BH_PrivateStart,/* not a state bit, but the first bit available
			 * for private allocation by other entities
			 */
};

#define MAX_BUF_PER_PAGE (PAGE_SIZE / 512)

struct page;
struct buffer_head;
struct address_space;
typedef void (bh_end_io_t)(struct buffer_head *bh, int uptodate);

/*
 * Historically, a buffer_head was used to map a single block
 * within a page, and of course as the unit of I/O through the
 * filesystem and block layers.  Nowadays the basic I/O unit
 * is the bio, and buffer_heads are used for extracting block
 * mappings (via a get_block_t call), for tracking state within
 * a page (via a page_mapping) and for wrapping bio submission
 * for backward compatibility reasons (e.g. submit_bh).
 */
/*历史上buffer_head用于映射页面中的单个块,如今是bio, 
  buffer_head用于提取块映射、跟踪页面内的状态以及向后兼容(如submit_bh)*/
struct buffer_head {
	unsigned long b_state;		/* buffer state bitmap (see above) 缓冲区的状态标志*/
	struct buffer_head *b_this_page;/* circular list of page's buffers 当前页中缓冲区列表*/
	struct page *b_page;		/* the page this bh is mapped to 当前缓冲区所在内存页*/

	sector_t b_blocknr;		/* start block number (文件系统上对应的物理块号/存储数据的块号)*/
	size_t b_size;			/* size of mapping buffer在内存中的大小*/
	char *b_data;			/* pointer to data within the page 数据在页中的位置,页内偏移*/

	struct block_device *b_bdev; //关联的块设备(通常是指磁盘或者是分区)
	bh_end_io_t *b_end_io;		/* I/O completion */
 	void *b_private;		/* reserved for b_end_io */
	struct list_head b_assoc_buffers; /* associated with another mapping 关联的其他缓冲区*/
	struct address_space *b_assoc_map;	/* mapping this buffer is
						   associated with 相关的地址空间address_space*/
	atomic_t b_count;		/* users using this buffer_head 缓冲区的使用计数*/
};

/*
 * macro tricks to expand the set_buffer_foo(), clear_buffer_foo()
 * and buffer_foo() functions.
 */
#define BUFFER_FNS(bit, name)						\
static __always_inline void set_buffer_##name(struct buffer_head *bh)	\
{									\
	set_bit(BH_##bit, &(bh)->b_state);				\
}									\
static __always_inline void clear_buffer_##name(struct buffer_head *bh)	\
{									\
	clear_bit(BH_##bit, &(bh)->b_state);				\
}									\
static __always_inline int buffer_##name(const struct buffer_head *bh)	\
{									\
	return test_bit(BH_##bit, &(bh)->b_state);			\
}

/*
 * test_set_buffer_foo() and test_clear_buffer_foo()
 */
#define TAS_BUFFER_FNS(bit, name)					\
static __always_inline int test_set_buffer_##name(struct buffer_head *bh) \
{									\
	return test_and_set_bit(BH_##bit, &(bh)->b_state);		\
}									\
static __always_inline int test_clear_buffer_##name(struct buffer_head *bh) \
{									\
	return test_and_clear_bit(BH_##bit, &(bh)->b_state);		\
}									\

/*
 * Emit the buffer bitops functions.   Note that there are also functions
 * of the form "mark_buffer_foo()".  These are higher-level functions which
 * do something in addition to setting a b_state bit.
 */
BUFFER_FNS(Uptodate, uptodate)
BUFFER_FNS(Dirty, dirty)
TAS_BUFFER_FNS(Dirty, dirty)
BUFFER_FNS(Lock, locked)
BUFFER_FNS(Req, req)
TAS_BUFFER_FNS(Req, req)
BUFFER_FNS(Mapped, mapped)
BUFFER_FNS(New, new)
BUFFER_FNS(Async_Read, async_read)
BUFFER_FNS(Async_Write, async_write)
BUFFER_FNS(Delay, delay)
BUFFER_FNS(Boundary, boundary)
BUFFER_FNS(Write_EIO, write_io_error)
BUFFER_FNS(Unwritten, unwritten)
BUFFER_FNS(Meta, meta)
BUFFER_FNS(Prio, prio)
BUFFER_FNS(Defer_Completion, defer_completion)

#define bh_offset(bh)		((unsigned long)(bh)->b_data & ~PAGE_MASK)

/* If we *know* page->private refers to buffer_heads */
#define page_buffers(page)					\
	({							\
		BUG_ON(!PagePrivate(page));			\
		((struct buffer_head *)page_private(page));	\
	})
#define page_has_buffers(page)	PagePrivate(page)

void buffer_check_dirty_writeback(struct page *page,
				     bool *dirty, bool *writeback);

/*
 * Declarations
 */

void mark_buffer_dirty(struct buffer_head *bh);
void mark_buffer_write_io_error(struct buffer_head *bh);
void init_buffer(struct buffer_head *, bh_end_io_t *, void *);
void touch_buffer(struct buffer_head *bh);
void set_bh_page(struct buffer_head *bh,
		struct page *page, unsigned long offset);
int try_to_free_buffers(struct page *);
struct buffer_head *alloc_page_buffers(struct page *page, unsigned long size,
		int retry);
void create_empty_buffers(struct page *, unsigned long,
			unsigned long b_state);
void end_buffer_read_sync(struct buffer_head *bh, int uptodate);
void end_buffer_write_sync(struct buffer_head *bh, int uptodate);
void end_buffer_async_write(struct buffer_head *bh, int uptodate);

/* Things to do with buffers at mapping->private_list */
void mark_buffer_dirty_inode(struct buffer_head *bh, struct inode *inode);
int inode_has_buffers(struct inode *);
void invalidate_inode_buffers(struct inode *);
int remove_inode_buffers(struct inode *inode);
int sync_mapping_buffers(struct address_space *mapping);
void clean_bdev_aliases(struct block_device *bdev, sector_t block,
			sector_t len);
static inline void clean_bdev_bh_alias(struct buffer_head *bh)
{
	clean_bdev_aliases(bh->b_bdev, bh->b_blocknr, 1);
}

void mark_buffer_async_write(struct buffer_head *bh);
void __wait_on_buffer(struct buffer_head *);
wait_queue_head_t *bh_waitq_head(struct buffer_head *bh);
struct buffer_head *__find_get_block(struct block_device *bdev, sector_t block,
			unsigned size);
struct buffer_head *__getblk_gfp(struct block_device *bdev, sector_t block,
				  unsigned size, gfp_t gfp);
void __brelse(struct buffer_head *);
void __bforget(struct buffer_head *);
void __breadahead(struct block_device *, sector_t block, unsigned int size);
struct buffer_head *__bread_gfp(struct block_device *,
				sector_t block, unsigned size, gfp_t gfp);
void invalidate_bh_lrus(void);
struct buffer_head *alloc_buffer_head(gfp_t gfp_flags);
void free_buffer_head(struct buffer_head * bh);
void unlock_buffer(struct buffer_head *bh);
void __lock_buffer(struct buffer_head *bh);
void ll_rw_block(int, int, int, struct buffer_head * bh[]);
int sync_dirty_buffer(struct buffer_head *bh);
int __sync_dirty_buffer(struct buffer_head *bh, int op_flags);
void write_dirty_buffer(struct buffer_head *bh, int op_flags);
int submit_bh(int, int, struct buffer_head *);
void write_boundary_block(struct block_device *bdev,
			sector_t bblock, unsigned blocksize);
int bh_uptodate_or_lock(struct buffer_head *bh);
int bh_submit_read(struct buffer_head *bh);
loff_t page_cache_seek_hole_data(struct inode *inode, loff_t offset,
				 loff_t length, int whence);

extern int buffer_heads_over_limit;

/*
 * Generic address_space_operations implementations for buffer_head-backed
 * address_spaces.
 */
void block_invalidatepage(struct page *page, unsigned int offset,
			  unsigned int length);
int block_write_full_page(struct page *page, get_block_t *get_block,
				struct writeback_control *wbc);
int __block_write_full_page(struct inode *inode, struct page *page,
			get_block_t *get_block, struct writeback_control *wbc,
			bh_end_io_t *handler);
int block_read_full_page(struct page*, get_block_t*);
int block_is_partially_uptodate(struct page *page, unsigned long from,
				unsigned long count);
int block_write_begin(struct address_space *mapping, loff_t pos, unsigned len,
		unsigned flags, struct page **pagep, get_block_t *get_block);
int __block_write_begin(struct page *page, loff_t pos, unsigned len,
		get_block_t *get_block);
int block_write_end(struct file *, struct address_space *,
				loff_t, unsigned, unsigned,
				struct page *, void *);
int generic_write_end(struct file *, struct address_space *,
				loff_t, unsigned, unsigned,
				struct page *, void *);
void page_zero_new_buffers(struct page *page, unsigned from, unsigned to);
void clean_page_buffers(struct page *page);
int cont_write_begin(struct file *, struct address_space *, loff_t,
			unsigned, unsigned, struct page **, void **,
			get_block_t *, loff_t *);
int generic_cont_expand_simple(struct inode *inode, loff_t size);
int block_commit_write(struct page *page, unsigned from, unsigned to);
int block_page_mkwrite(struct vm_area_struct *vma, struct vm_fault *vmf,
				get_block_t get_block);
/* Convert errno to return value from ->page_mkwrite() call */
static inline int block_page_mkwrite_return(int err)
{
	if (err == 0)
		return VM_FAULT_LOCKED;
	if (err == -EFAULT || err == -EAGAIN)
		return VM_FAULT_NOPAGE;
	if (err == -ENOMEM)
		return VM_FAULT_OOM;
	/* -ENOSPC, -EDQUOT, -EIO ... */
	return VM_FAULT_SIGBUS;
}
sector_t generic_block_bmap(struct address_space *, sector_t, get_block_t *);
int block_truncate_page(struct address_space *, loff_t, get_block_t *);
int nobh_write_begin(struct address_space *, loff_t, unsigned, unsigned,
				struct page **, void **, get_block_t*);
int nobh_write_end(struct file *, struct address_space *,
				loff_t, unsigned, unsigned,
				struct page *, void *);
int nobh_truncate_page(struct address_space *, loff_t, get_block_t *);
int nobh_writepage(struct page *page, get_block_t *get_block,
                        struct writeback_control *wbc);

void buffer_init(void);

/*
 * inline definitions
 */

static inline void attach_page_buffers(struct page *page,
		struct buffer_head *head)
{
	get_page(page);
	SetPagePrivate(page);//设置PG_private标志，通知内核其他部分，page实例的private成员正在使用中 
	set_page_private(page, (unsigned long)head);
}

static inline void get_bh(struct buffer_head *bh)
{
        atomic_inc(&bh->b_count);
}

static inline void put_bh(struct buffer_head *bh)
{
        smp_mb__before_atomic();
        atomic_dec(&bh->b_count);
}

static inline void brelse(struct buffer_head *bh)
{
	if (bh)
		__brelse(bh);
}

static inline void bforget(struct buffer_head *bh)
{
	if (bh)
		__bforget(bh);
}

static inline struct buffer_head *
sb_bread(struct super_block *sb, sector_t block)
{
	return __bread_gfp(sb->s_bdev, block, sb->s_blocksize, __GFP_MOVABLE);
}

static inline struct buffer_head *
sb_bread_unmovable(struct super_block *sb, sector_t block)
{
	return __bread_gfp(sb->s_bdev, block, sb->s_blocksize, 0);
}

static inline void
sb_breadahead(struct super_block *sb, sector_t block)
{
	__breadahead(sb->s_bdev, block, sb->s_blocksize);
}

static inline struct buffer_head *
sb_getblk(struct super_block *sb, sector_t block)
{
	return __getblk_gfp(sb->s_bdev, block, sb->s_blocksize, __GFP_MOVABLE);
}


static inline struct buffer_head *
sb_getblk_gfp(struct super_block *sb, sector_t block, gfp_t gfp)
{
	return __getblk_gfp(sb->s_bdev, block, sb->s_blocksize, gfp);
}

static inline struct buffer_head *
sb_find_get_block(struct super_block *sb, sector_t block)
{
	return __find_get_block(sb->s_bdev, block, sb->s_blocksize);
}

static inline void
map_bh(struct buffer_head *bh, struct super_block *sb, sector_t block)
{
	set_buffer_mapped(bh);
	bh->b_bdev = sb->s_bdev;
	bh->b_blocknr = block;
	bh->b_size = sb->s_blocksize;
}

static inline void wait_on_buffer(struct buffer_head *bh)
{
	might_sleep();
	if (buffer_locked(bh))
		__wait_on_buffer(bh);
}

static inline int trylock_buffer(struct buffer_head *bh)
{
	return likely(!test_and_set_bit_lock(BH_Lock, &bh->b_state));
}

static inline void lock_buffer(struct buffer_head *bh)
{
	might_sleep();
	if (!trylock_buffer(bh))
		__lock_buffer(bh);
}

static inline struct buffer_head *getblk_unmovable(struct block_device *bdev,
						   sector_t block,
						   unsigned size)
{
	return __getblk_gfp(bdev, block, size, 0);
}

static inline struct buffer_head *__getblk(struct block_device *bdev,
					   sector_t block,
					   unsigned size)
{
	return __getblk_gfp(bdev, block, size, __GFP_MOVABLE);
}

/**
 *  __bread() - reads a specified block and returns the bh
 *  @bdev: the block_device to read from
 *  @block: number of block
 *  @size: size (in bytes) to read
 *
 *  Reads a specified block, and returns buffer head that contains it.
 *  The page cache is allocated from movable area so that it can be migrated.
 *  It returns NULL if the block was unreadable.
 */
static inline struct buffer_head *
__bread(struct block_device *bdev, sector_t block, unsigned size)
{
	return __bread_gfp(bdev, block, size, __GFP_MOVABLE);
}

extern int __set_page_dirty_buffers(struct page *page);

#else /* CONFIG_BLOCK */

static inline void buffer_init(void) {}
static inline int try_to_free_buffers(struct page *page) { return 1; }
static inline int inode_has_buffers(struct inode *inode) { return 0; }
static inline void invalidate_inode_buffers(struct inode *inode) {}
static inline int remove_inode_buffers(struct inode *inode) { return 1; }
static inline int sync_mapping_buffers(struct address_space *mapping) { return 0; }

#endif /* CONFIG_BLOCK */
#endif /* _LINUX_BUFFER_HEAD_H */
