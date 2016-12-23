/*
** 2001 September 16
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This header file (together with is companion C source-code file
** "os.c") attempt to abstract the underlying operating system so that
** the SQLite library will work on both POSIX and windows systems.
** 
** 这个头文件（和C语言os.c文件一起）用于抽象
** 操作系统，这样SQLite文件就可以工作在POSIX和windows系统
**
** This header file is #include-ed by sqliteInt.h and thus ends up
** being included by every source file.
**
** 这个头文件包含一些sqliteInt.h头文件，所以
** 被所有引用的文件所包含
*/
#ifndef _SQLITE_OS_H_
#define _SQLITE_OS_H_

/*
** Figure out if we are dealing with Unix, Windows, or some other
** operating system.  After the following block of preprocess macros,
** all of SQLITE_OS_UNIX, SQLITE_OS_WIN, and SQLITE_OS_OTHER 
** will defined to either 1 or 0.  One of the four will be 1.  The other 
** three will be 0.
** 假设我们需要处理Unix, Windows或者其他操作系统
** 所以接下来我们定义了相关宏参数，包括
** SQLITE_OS_UNIX, SQLITE_OS_WIN, SQLITE_OS_OTHER，各自只能
** 是1或0。四个中只有一个是，其他的都是0
*/
#if defined(SQLITE_OS_OTHER)
# if SQLITE_OS_OTHER==1
#   undef SQLITE_OS_UNIX
#   define SQLITE_OS_UNIX 0
#   undef SQLITE_OS_WIN
#   define SQLITE_OS_WIN 0
# else
#   undef SQLITE_OS_OTHER
# endif
#endif
#if !defined(SQLITE_OS_UNIX) && !defined(SQLITE_OS_OTHER)
# define SQLITE_OS_OTHER 0
# ifndef SQLITE_OS_WIN
#   if defined(_WIN32) || defined(WIN32) || defined(__CYGWIN__) || defined(__MINGW32__) || defined(__BORLANDC__)
#     define SQLITE_OS_WIN 1
#     define SQLITE_OS_UNIX 0
#   else
#     define SQLITE_OS_WIN 0
#     define SQLITE_OS_UNIX 1
#  endif
# else
#  define SQLITE_OS_UNIX 0
# endif
#else
# ifndef SQLITE_OS_WIN
#  define SQLITE_OS_WIN 0
# endif
#endif

#if SQLITE_OS_WIN
# include <windows.h>
#endif

/*
** Determine if we are dealing with Windows NT.
**
** 假设我们在处理Windows NT, 我们需要处理win98或winNT
** 我们使用_WIN32_WINNT宏定义
**
** We ought to be able to determine if we are compiling for win98 or winNT
** using the _WIN32_WINNT macro as follows:
**
** #if defined(_WIN32_WINNT)
** # define SQLITE_OS_WINNT 1
** #else
** # define SQLITE_OS_WINNT 0
** #endif
**
** 然而，vs2005在默认情况下不使用_WIN32_WINNT，
** 虽然它本应该使用。所以上面的测试不起作用
** 我们假设都是winNT，除非程序特殊说明或设置
** SQLITE_OS_WINNT为0
** 
** However, vs2005 does not set _WIN32_WINNT by default, as it ought to,
** so the above test does not work.  We'll just assume that everything is
** winNT unless the programmer explicitly says otherwise by setting
** SQLITE_OS_WINNT to 0.
*/
#if SQLITE_OS_WIN && !defined(SQLITE_OS_WINNT)
# define SQLITE_OS_WINNT 1
#endif

/*
** Determine if we are dealing with WindowsCE - which has a much
** reduced API.
**
** 假设我们处理WindowsCE，拥有简化的API
*/
#if defined(_WIN32_WCE)
# define SQLITE_OS_WINCE 1
#else
# define SQLITE_OS_WINCE 0
#endif

/*
** Determine if we are dealing with WinRT, which provides only a subset of
** the full Win32 API.
** 
#if !defined(SQLITE_OS_WINRT)
** 假设我们处理WinRT，仅仅提供了全部的Win32 API
*/
#if !defined(SQLITE_OS_WINRT)
# define SQLITE_OS_WINRT 0
#endif

/*
** When compiled for WinCE or WinRT, there is no concept of the current
** directory.
**
** 在WinCE或WinRT上编译的时候，当前目录没有内容
 */
#if !SQLITE_OS_WINCE && !SQLITE_OS_WINRT
# define SQLITE_CURDIR 1
#endif

/* If the SET_FULLSYNC macro is not defined above, then make it
** a no-op
**
** 如果上面没有定义SET_FULLSYNC宏，值为no-op
*/
#ifndef SET_FULLSYNC
# define SET_FULLSYNC(x,y)
#endif

/*
** The default size of a disk sector
**
** 默认存储单元的大小
*/
#ifndef SQLITE_DEFAULT_SECTOR_SIZE
# define SQLITE_DEFAULT_SECTOR_SIZE 4096
#endif

/*
** Temporary files are named starting with this prefix followed by 16 random
** alphanumeric characters, and no file extension. They are stored in the
** OS's standard temporary file directory, and are deleted prior to exit.
** If sqlite is being embedded in another program, you may wish to change the
** prefix to reflect your program's name, so that if your program exits
** prematurely, old temporary files can be easily identified. This can be done
** using -DSQLITE_TEMP_FILE_PREFIX=myprefix_ on the compiler command line.
**
** 临时文件的命名都是以16位随机字符开头，并且没有文件格式
** 说明，都是存放在系统临时文件夹，在退出时删除。
** 在sqlite嵌入到另一个软件中时，你可能希望改变预设值，从而
** 可以持久保存，所以旧的临时文件就可以简单的分辨开。
** 这些可以通过在编译的时候使用-DSQLITE_TEMP_FILE_PREFIX=myprefix_。
**
** 2006-10-31:  The default prefix used to be "sqlite_".  But then
** Mcafee started using SQLite in their anti-virus product and it
** started putting files with the "sqlite" name in the c:/temp folder.
** This annoyed many windows users.  Those users would then do a 
** Google search for "sqlite", find the telephone numbers of the
** developers and call to wake them up at night and complain.
** For this reason, the default name prefix is changed to be "sqlite" 
** spelled backwards.  So the temp files are still identified, but
** anybody smart enough to figure out the code is also likely smart
** enough to know that calling the developer will not help get rid
** of the file.
**
** 2006-10-31：在预设的情况下使用"sqlite_". 但是由于Mcafee开始在自己的
** 安全产品中使用SQLite，同时将"sqlite"临时文件存放到"c:/temp"文件夹
** 而这些就会导致Windows用户的误解，在Google上搜索"sqlite",找到开发
** 人员的联系方式，在深夜的时候喊他们起床，抱怨这个问题。
** 因此，默认的名称变成以"sqlite"结尾，临时文件仍然可以被区分。
** 所以那些可以找到开发人员联系方式的人们将不再会抱怨这个问题
** 
*/
#ifndef SQLITE_TEMP_FILE_PREFIX
# define SQLITE_TEMP_FILE_PREFIX "etilqs_"
#endif

/*
** The following values may be passed as the second argument to
** sqlite3OsLock(). The various locks exhibit the following semantics:
**
** 接下来的值将会作为sqlite3OsLock()的第二声明，接下来的部分
** 将会展示各种各样的锁结构。
** SHARED: 任何数量的进程都可以同时拥有一个对象的SHARED锁
** RESERVED:单独的进程在任何时候都可以持有一个RESERVED锁，
** 其他的进程都只能拥有一个SHARED锁。
** PENDING:一个单独的进程可以在任何时候拥有一个PENDING锁
** 已存的SHARED锁可以一直存在，但是其它的进程不能获得
** 新的PENDING锁.
** EXCLUSIVE:一个EXCLUSIVE锁不允许进程拥有其他的锁。
** PENDING_LOCK可能不能直接通过sqlite3OsLock()，相反，一个请求EXCLUSVE锁的
** 进程可以获得PENDING锁。所以可以通过请求sqlite3OsLock()提升EXCLUSIVE锁
**
** SHARED:    Any number of processes may hold a SHARED lock simultaneously.
** RESERVED:  A single process may hold a RESERVED lock on a file at
**            any time. Other processes may hold and obtain new SHARED locks.
** PENDING:   A single process may hold a PENDING lock on a file at
**            any one time. Existing SHARED locks may persist, but no new
**            SHARED locks may be obtained by other processes.
** EXCLUSIVE: An EXCLUSIVE lock precludes all other locks.
**
** PENDING_LOCK may not be passed directly to sqlite3OsLock(). Instead, a
** process that requests an EXCLUSIVE lock may actually obtain a PENDING
** lock. This can be upgraded to an EXCLUSIVE lock by a subsequent call to
** sqlite3OsLock().
*/
#define NO_LOCK         0
#define SHARED_LOCK     1
#define RESERVED_LOCK   2
#define PENDING_LOCK    3
#define EXCLUSIVE_LOCK  4

/*
** File Locking Notes:  (Mostly about windows but also some info for Unix)
**
** 文件锁备注：大多数和windows相关，也有部分适用于Unix
**
** We cannot use LockFileEx() or UnlockFileEx() on Win95/98/ME because
** those functions are not available.  So we use only LockFile() and
** UnlockFile().
**
** 我们不能在Win95/98/ME上使用LockFileEx()或UnlockFileEx()函数，这是
** 由于这些方法不可用，所以只能使用LockFile()和UnlockFile()
**
** LockFile() prevents not just writing but also reading by other processes.
** A SHARED_LOCK is obtained by locking a single randomly-chosen 
** byte out of a specific range of bytes. The lock byte is obtained at 
** random so two separate readers can probably access the file at the 
** same time, unless they are unlucky and choose the same lock byte.
** An EXCLUSIVE_LOCK is obtained by locking all bytes in the range.
** There can only be one writer.  A RESERVED_LOCK is obtained by locking
** a single byte of the file that is designated as the reserved lock byte.
** A PENDING_LOCK is obtained by locking a designated byte different from
** the RESERVED_LOCK byte.b
**
** LockFile()可以阻止其他进程写和读，一个SHARED_LOCK获得需要改变
** 随机选取的标志位，而由于它是随机选取，所以两个不同的读者
** 可以同时获取文件的句柄。除非它们不幸的选取了同一个标志位
** 一个EXCLUSIVE_LOCK通过锁定某区域内所有位，只能拥有一个写者，
** 一个RESERVED_LOCK通过锁定文件中预留的单比特位。
** 一个PENDING_LOCK通过锁定和RESERVED_LOCK不同的位获得该锁。
**
** On WinNT/2K/XP systems, LockFileEx() and UnlockFileEx() are available,
** which means we can use reader/writer locks.  When reader/writer locks
** are used, the lock is placed on the same range of bytes that is used
** for probabilistic locking in Win95/98/ME.  Hence, the locking scheme
** will support two or more Win95 readers or two or more WinNT readers.
** But a single Win95 reader will lock out all WinNT readers and a single
** WinNT reader will lock out all other Win95 readers.
**
** 在WinNT/2K/XP系统中，LockFileEx()和UnlockFileEx()都是可用的，意味着我们可以
** 使用读和写锁。在使用这些锁的时候，和Win95/98/ME上锁定的标志位
** 相同。因此，锁模式在Win95/WinNT上可以支持更多的写进程。但是一个
** Win95读进程会锁定其它WinNT读进程，WinNT读进程也会锁住其他Win95读进程
**
** The following #defines specify the range of bytes used for locking.
** SHARED_SIZE is the number of bytes available in the pool from which
** a random byte is selected for a shared lock.  The pool of bytes for
** shared locks begins at SHARED_FIRST. 
**
** 接下来的#define都是用于锁定特殊字节，SHARED_SIZE是锁定字节池中随机
** 锁定的区域，从而获得shard锁，这个锁定字节池以SHARED_FIRST开始。
**
** The same locking strategy and
** byte ranges are used for Unix.  This leaves open the possiblity of having
** clients on win95, winNT, and unix all talking to the same shared file
** and all locking correctly.  To do so would require that samba (or whatever
** tool is being used for file sharing) implements locks correctly between
** windows and unix.  I'm guessing that isn't likely to happen, but by
** using the same locking range we are at least open to the possibility.
**
** 而在Unix中锁定策略和区域也是一样的。这个策略使得在Win95. WinNT, unix
** 平台上使用同样的共享文件夹，有利于维护锁的正确性。而要做到这点
** 需要使用samba(或者其他用于文件共享的工具）以保持两个系统间的正确
** 我估计这个现象可能不会出现，但是使用同样的区域至少可以让我们
** 在打开文件时保持正确。
**
** Locking in windows is manditory.  For this reason, we cannot store
** actual data in the bytes used for locking.  The pager never allocates
** the pages involved in locking therefore.  SHARED_SIZE is selected so
** that all locks will fit on a single page even at the minimum page size.
** PENDING_BYTE defines the beginning of the locks.  By default PENDING_BYTE
** is set high so that we don't have to allocate an unused page except
** for very large databases.  But one should test the page skipping logic 
** by setting PENDING_BYTE low and running the entire regression suite.
**
** 由于Windows下锁是强制性的，所以我们不能在锁标志位上存储有意义的值
** 页面也从不使用此前锁定的页面。SHARED_SIZE大小的选择恰好为一个最小页
** 面的大小。PENDDING_BYTE定义了锁区间的起始位。PENDING_BYTE在默认情况下设
** 置的位置在高位，所以除非在特别大的数据库，我们不需要改变这个值。
** 但是我们需要除设置PENDING_BYTE进行逻辑测试之外，还需要执行完整的测试。
**
** Changing the value of PENDING_BYTE results in a subtly incompatible
** file format.  Depending on how it is changed, you might not notice
** the incompatibility right away, even running a full regression test.
** The default location of PENDING_BYTE is the first byte past the
** 1GB boundary.
**
** 改变PENDING_BYTE的值会导致和文件格式产生隐形错误。这些错误取决于怎么
** 被改变的，用户可能不会很明显的注意到这些改变，甚至在一些完整的测
** 试中也不会发现。默认的PENDING_BYTE位于1GB内存的第一个字节
**
*/
#ifdef SQLITE_OMIT_WSD
# define PENDING_BYTE     (0x40000000)
#else
# define PENDING_BYTE      sqlite3PendingByte
#endif
#define RESERVED_BYTE     (PENDING_BYTE+1)
#define SHARED_FIRST      (PENDING_BYTE+2)
#define SHARED_SIZE       510

/*
** Wrapper around OS specific sqlite3_os_init() function.
**
** 封装sqlite3_os_init()中所以和OS相关函数
*/
int sqlite3OsInit(void);

/* 
** Functions for accessing sqlite3_file methods 
**
** 所有sqlite3_file文件相关函数
*/
int sqlite3OsClose(sqlite3_file*);
int sqlite3OsRead(sqlite3_file*, void*, int amt, i64 offset);
int sqlite3OsWrite(sqlite3_file*, const void*, int amt, i64 offset);
int sqlite3OsTruncate(sqlite3_file*, i64 size);
int sqlite3OsSync(sqlite3_file*, int);
int sqlite3OsFileSize(sqlite3_file*, i64 *pSize);
int sqlite3OsLock(sqlite3_file*, int);
int sqlite3OsUnlock(sqlite3_file*, int);
int sqlite3OsCheckReservedLock(sqlite3_file *id, int *pResOut);
int sqlite3OsFileControl(sqlite3_file*,int,void*);
void sqlite3OsFileControlHint(sqlite3_file*,int,void*);
#define SQLITE_FCNTL_DB_UNCHANGED 0xca093fa0
int sqlite3OsSectorSize(sqlite3_file *id);
int sqlite3OsDeviceCharacteristics(sqlite3_file *id);
int sqlite3OsShmMap(sqlite3_file *,int,int,int,void volatile **);
int sqlite3OsShmLock(sqlite3_file *id, int, int, int);
void sqlite3OsShmBarrier(sqlite3_file *id);
int sqlite3OsShmUnmap(sqlite3_file *id, int);


/* 
** Functions for accessing sqlite3_vfs methods 
**
** 访问sqlite3_vfs方法
*/
int sqlite3OsOpen(sqlite3_vfs *, const char *, sqlite3_file*, int, int *);
int sqlite3OsDelete(sqlite3_vfs *, const char *, int);
int sqlite3OsAccess(sqlite3_vfs *, const char *, int, int *pResOut);
int sqlite3OsFullPathname(sqlite3_vfs *, const char *, int, char *);
#ifndef SQLITE_OMIT_LOAD_EXTENSION
void *sqlite3OsDlOpen(sqlite3_vfs *, const char *);
void sqlite3OsDlError(sqlite3_vfs *, int, char *);
void (*sqlite3OsDlSym(sqlite3_vfs *, void *, const char *))(void);
void sqlite3OsDlClose(sqlite3_vfs *, void *);
#endif /* SQLITE_OMIT_LOAD_EXTENSION */
int sqlite3OsRandomness(sqlite3_vfs *, int, char *);
int sqlite3OsSleep(sqlite3_vfs *, int);
int sqlite3OsCurrentTimeInt64(sqlite3_vfs *, sqlite3_int64*);

/*
** Convenience functions for opening and closing files using 
** sqlite3_malloc() to obtain space for the file-handle structure.
**
** 在sqlite3_malloc()中获取打开和关闭文件的句柄
*/
int sqlite3OsOpenMalloc(sqlite3_vfs *, const char *, sqlite3_file **, int,int*);
int sqlite3OsCloseFree(sqlite3_file *);

#endif /* _SQLITE_OS_H_ */
