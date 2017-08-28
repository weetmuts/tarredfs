/*
 Copyright (C) 2016-2017 Fredrik Öhrström

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include"log.h"
#include"tar.h"
#include<sys/stat.h>
#include<string.h>
#include<assert.h>

#define T_NAMELEN		100
#define T_LINKLEN               100

#define REGTYPE  '0'
#define LNKTYPE  '1'
#define SYMTYPE  '2'
#define CHRTYPE  '3'
#define BLKTYPE  '4'
#define DIRTYPE  '5'
#define FIFOTYPE '6'

#define GNU_LONGNAME_TYPE	'L'
#define GNU_LONGLINK_TYPE	'K'
#define GNU_VOLHDR_TYPE	        'V'

#define TSUID    04000
#define TSGID    02000
#define TSVTX    01000
#define TUREAD   00400
#define TUWRITE  00200
#define TUEXEC   00100
#define TGREAD   00040
#define TGWRITE  00020
#define TGEXEC   00010
#define TOREAD   00004
#define TOWRITE  00002
#define TOEXEC   00001

static_assert (sizeof(TarHeaderContents) == T_BLOCKSIZE, "TarHeaderContents size is not correct!");

static ComponentId TAR = registerLogComponent("tar");

bool store_path_(Path *path, char *name_str, size_t nlen) {
    const char *tp = path->c_str();
    size_t path_len = path->c_str_len();

    if (path_len <= nlen)
    {
	// Entire path can be stored in name.
	if (name_str) strncpy(name_str, tp, nlen);
	return true;
    }

    return false;
}

size_t calculate_header_size_(struct stat *sb, Path *tarpath, Path *link,
		       char *name_str, char *link_str,
		       size_t *num_long_path_blocks,
		       size_t *num_long_link_blocks,
		       size_t *num_header_blocks)
{
    *num_long_path_blocks = 0;
    *num_long_link_blocks = 0;
    *num_header_blocks = 1;
    
    bool name_fits = store_path_(tarpath, name_str, T_NAMELEN);

    if (!name_fits) {
        // We needed to use gnu long names, aka an extra header block
        // plus at least one block for the file name. A path longer than 512
        // bytes will need a third block etc
        *num_long_path_blocks = 2 + tarpath->c_str_len()/T_BLOCKSIZE;
        *num_header_blocks += *num_long_path_blocks;
        debug(TAR, "Added %ju blocks for long path header for %s\n", num_long_path_blocks, tarpath->c_str());
    }
	
    bool link_fits = true;
    if (link) {
	// We have a link to store, does it fit in the first header block?
	link_fits = store_path_(link, link_str, T_LINKLEN);
	if (!link_fits) {
            // We needed to use gnu long links, aka an extra header block
            // plus at least one block for the file name. A link target path longer than 512
            // bytes will need a third block etc
            *num_long_link_blocks = 2 + link->c_str_len()/T_BLOCKSIZE;
            *num_header_blocks += *num_long_link_blocks;
            debug(TAR, "Added %ju blocks for long link header for %s\n", num_long_link_blocks, link->c_str());
	}
    }    
    
    return (*num_header_blocks)*T_BLOCKSIZE;
}

size_t TarHeader::calculateSize(struct stat *sb, Path *tarpath, Path *link, bool is_hard_link)
{
    size_t num_long_path_blocks;
    size_t num_long_link_blocks;
    size_t num_header_blocks;
    if (link && is_hard_link) {
	link = link->unRoot();
    }
    return calculate_header_size_(sb, tarpath, link,
				  NULL, NULL,
				  &num_long_path_blocks,
				  &num_long_link_blocks,
				  &num_header_blocks);
}    

void TarHeader::setLongLinkType(TarHeader *file)
{
    memcpy(&content, &file->content, sizeof(content));
    snprintf(content.members.mtime_, 12, "%011zo", (size_t)0);
    content.members.typeflag_ = GNU_LONGLINK_TYPE;
    strcpy(content.members.name_, "././@LongLink");
}

void TarHeader::setLongPathType(TarHeader *file)
{
    memcpy(&content, &file->content, sizeof(content));
    snprintf(content.members.mtime_, 12, "%011zo", (size_t)0);
    content.members.typeflag_ = GNU_LONGNAME_TYPE;
    strcpy(content.members.name_, "././@LongLink");
}

void TarHeader::setSize(size_t s)
{
    snprintf(content.members.size_, 12, "%011zo", s);
}

char getTypeFlagFrom(struct stat *sb, bool is_hard_link)
{
    // LNKTYPE in the tar spec means hard link!
    // This must be tested first....
    if (is_hard_link) return LNKTYPE;
    // Whereas libc macro S_ISLNK returns true for SYMBOLIC links!
    if (S_ISLNK(sb->st_mode)) return SYMTYPE;
    
    if (S_ISREG(sb->st_mode)) return REGTYPE;
    if (S_ISCHR(sb->st_mode)) return CHRTYPE;
    if (S_ISBLK(sb->st_mode)) return BLKTYPE;
    if (S_ISDIR(sb->st_mode)) return DIRTYPE;
    if (S_ISFIFO(sb->st_mode)) return FIFOTYPE;    


    assert(0);
}

void writeModeFlagFrom(struct stat *sb, char *mode) {
    int bits = 0;
    if (sb->st_mode & S_ISUID) bits |= TSUID;
    if (sb->st_mode & S_ISGID) bits |= TSGID;
    if (sb->st_mode & S_ISVTX) bits |= TSVTX; // Sticky bit

    if (sb->st_mode & S_IRUSR) bits |= TUREAD;
    if (sb->st_mode & S_IWUSR) bits |= TUWRITE;
    if (sb->st_mode & S_IXUSR) bits |= TUEXEC;

    if (sb->st_mode & S_IRGRP) bits |= TGREAD;
    if (sb->st_mode & S_IWGRP) bits |= TGWRITE;
    if (sb->st_mode & S_IXGRP) bits |= TGEXEC;

    if (sb->st_mode & S_IROTH) bits |= TOREAD;
    if (sb->st_mode & S_IWOTH) bits |= TOWRITE;
    if (sb->st_mode & S_IXOTH) bits |= TOEXEC;

    snprintf(mode, 8, "%07o", bits);
}


TarHeader::TarHeader()
{
    memset(&content.members, 0, sizeof(content.members)); 
    num_long_path_blocks_ = 0;
    num_long_link_blocks_ = 0;
    num_header_blocks_ = 0;
}

TarHeader::TarHeader(struct stat *sb, Path *tarpath, Path *link, bool is_hard_link)
{
    memset(&content.members, 0, sizeof(content.members));

    if (link && is_hard_link) {
	link = link->unRoot();
    }    
    calculate_header_size_(sb, tarpath, link,
			   content.members.name_,
			   content.members.linkname_,
			   &num_long_path_blocks_,
			   &num_long_link_blocks_,
			   &num_header_blocks_);
    
    // Mode
    writeModeFlagFrom(sb, content.members.mode_);

    // uid gid
    memcpy(content.members.uid_, "0000000", 8);
    memcpy(content.members.gid_, "0000000", 8);

    // size
    size_t s = 0;
    if (S_ISREG(sb->st_mode)) s = sb->st_size;
    snprintf(content.members.size_, 12, "%011zo", s);

    // mtime
    snprintf(content.members.mtime_, 12, "%011zo", sb->st_mtim.tv_sec);

    // checksum, to be filled in later.
    memset(content.members.checksum_, ' ', 8);

    // typeflag
    content.members.typeflag_ = getTypeFlagFrom(sb, is_hard_link);

    // linkname

    // magic and version GNU format
    memcpy(content.members.magic_, "ustar ", 6);
    memcpy(content.members.version_, " ", 2);

    // user name and group name
    memcpy(content.members.uname_, "beak", 5);
    memcpy(content.members.gname_, "beak", 5);

    calculateChecksum();
}

void TarHeader::calculateChecksum() {
    memset(content.members.checksum_, 32, 8);

    unsigned int checksum = 0;
    for (int i=0; i<512; ++i) {
	checksum += content.buf[i];
    }

    snprintf(content.members.checksum_, 8, "%07o", checksum);
}


