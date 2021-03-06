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

#ifndef INDEX_H
#define INDEX_H

#include "util.h"
#include "tarfile.h"

#include <functional>
#include <set>
#include <string>

struct IndexEntry {
    FileStat fs;
    size_t offset;
    std::string tarr;
    Path *path;
    std::string link;
    bool is_sym_link;
    bool is_hard_link;
    uint num_parts;
    size_t part_offset;
    size_t part_size;
    size_t last_part_size;
    size_t ondisk_part_size;
    size_t ondisk_last_part_size;

    size_t contentSize(size_t partnr)
    {
        if (partnr == num_parts-1) return last_part_size;
        return part_size;
    }

    size_t diskSize(size_t partnr)
    {
        if (partnr == num_parts-1) return ondisk_last_part_size;
        return ondisk_part_size;
    }
};

struct IndexTar {
    Path *backup_location;
    Path *tarfile_location;
    TarFileName from, to;
};

struct Index {
    static RC loadIndex(std::vector<char> &contents,
                         std::vector<char>::iterator &i,
                         IndexEntry *tmpentry, IndexTar *tmptar,
                         Path *dir_to_prepend,
                         Path *safedir_to_prepend,
                         size_t *size,
                         std::function<void(IndexEntry*)> on_entry,
                         std::function<void(IndexTar*)> on_tar);
};

#endif
