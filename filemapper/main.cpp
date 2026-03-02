#include <algorithm>
#include <vector>
#include <string>
#include <random>
#include <filesystem>
#include <iostream>
#include <cstdio>
#include <ctime>
#include <cstdint>
#include <cinttypes>
#include <cstring>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <filesystem>

using namespace std;
namespace fs = std::filesystem;
using u64 = uint64_t;

static size_t page_align(size_t x) {
    size_t p = sysconf(_SC_PAGE_SIZE);
    return (x + p - 1) & ~(p - 1);
}

static off_t get_file_size(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) {
        return -1;
    }
    return st.st_size;
}

string make_temp_filename() {
    string tmpl = "runXXXXXX";
    vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = mkstemp(buf.data());
    if (fd >= 0) {
        close(fd);
    }
    return string(buf.data());
}

struct MappedReader {
    int fd = -1;
    off_t file_size = 0; // bytes
    off_t offset = 0; // next byte offset in file to map when remapping
    size_t window_bytes = 0; // how many bytes to map each window

    void* data = MAP_FAILED;
    size_t mapped_bytes = 0; // bytes currently mapped
    size_t idx_in_mapped = 0; // index of next u64 in mapped
    size_t mapped_u64_count = 0;

    bool eof = false;

    MappedReader() {}
    ~MappedReader() { close_map(); if (fd>=0) close(fd); }

    bool open_file(const string &path, size_t window_bytes_) {
        fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            return false;
        }
        file_size = get_file_size(fd);
        window_bytes = window_bytes_;
        offset = 0;
        return remap();
    }

    bool remap() {
        close_map();
        if (offset >= file_size) {
             eof = true; return false; 
        }
        off_t bytes_left = file_size - offset;
        mapped_bytes = (size_t)std::min<off_t>(bytes_left, (off_t)window_bytes);
        size_t aligned = page_align(mapped_bytes);
        // align mapping offset to page size
        off_t map_offset = (offset / sysconf(_SC_PAGE_SIZE)) * sysconf(_SC_PAGE_SIZE);
        off_t offset_diff = offset - map_offset;
        size_t map_len = aligned + offset_diff;
        if (map_len == 0) { 
            eof = true; return false; 
        }
        data = mmap(nullptr, map_len, PROT_READ, MAP_SHARED, fd, map_offset);
        if (data == MAP_FAILED) {
            perror("mmap");
            return false;
        }
        // set pointers appropriately
        void* start_ptr = (char*)data + offset_diff;
        mapped_u64_count = mapped_bytes / sizeof(u64);
        idx_in_mapped = 0;
        eof = (mapped_u64_count == 0);
        return !eof;
    }

    void close_map() {
        if (data != MAP_FAILED) {
            size_t map_len = page_align(mapped_bytes + 0);
            munmap(data, map_len);
            data = MAP_FAILED;
        }
    }

    bool peek(u64 &out) {
        if (eof) { return false; }
        if (data == MAP_FAILED) { return false; }
        u64 *arr = (u64 *)((char*)data);
        out = arr[idx_in_mapped];
        return true;
    }

    bool advance() {
        idx_in_mapped++;
        if (idx_in_mapped >= mapped_u64_count) {
            // move offset forward by mapped_bytes
            offset += mapped_bytes;
            if (offset >= file_size) { 
                eof = true; 
                return false; 
            }
            // remap next window
            close_map();
            // map next window exactly window_bytes or remainder
            off_t bytes_left = file_size - offset;
            mapped_bytes = (size_t)std::min<off_t>(bytes_left, (off_t)window_bytes);
            size_t aligned = page_align(mapped_bytes);
            off_t map_offset = (offset / sysconf(_SC_PAGE_SIZE)) * sysconf(_SC_PAGE_SIZE);
            off_t offset_diff = offset - map_offset;
            size_t map_len = aligned + offset_diff;
            data = mmap(nullptr, map_len, PROT_READ, MAP_SHARED, fd, map_offset);
            if (data == MAP_FAILED) { 
                eof = true;
                return false; 
            }
            // reset idx_in_mapped and mapped_u64_count
            u64 *arr = (u64 *)((char*)data + offset_diff);
            mapped_u64_count = mapped_bytes / sizeof(u64);
            idx_in_mapped = 0;
            return true;
        }
        return true;
    }

    bool pop(u64 &out) {
        if (eof) { return false;}
        if (data == MAP_FAILED) { return false; }
        u64 *arr = (u64 *)((char*)data);
        out = arr[idx_in_mapped];
        return advance();
    }
};

struct SimpleMappedReader {
    int fd = -1;
    off_t file_size = 0;
    off_t pos = 0; // next byte position to read
    void* map_base = MAP_FAILED;
    off_t map_offset = 0; // aligned
    size_t map_len = 0; // length of current mapping
    u64* arr = nullptr; // pointer to first u64 in current window (taking offset diff into account)
    size_t u64_count = 0;
    size_t idx = 0;
    size_t window_bytes = 0;

    ~SimpleMappedReader() { close_fd(); }
    void close_fd() {
        if (map_base != MAP_FAILED) {
            munmap(map_base, map_len);
            map_base = MAP_FAILED;
        }
        if (fd >= 0) {
            close(fd);
        }
        fd = -1;
    }
    bool open_file(const string &path, size_t window_bytes_) {
        fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            return false;
        }
        file_size = get_file_size(fd);
        pos = 0;
        map_base = MAP_FAILED;
        window_bytes = window_bytes_;
        return map_next();
    }
    bool map_next() {
        if (map_base != MAP_FAILED) { munmap(map_base, map_len); map_base = MAP_FAILED; }
        if (pos >= file_size) {
            return false;
        }
        off_t bytes_left = file_size - pos;
        size_t req = (size_t)std::min<off_t>(bytes_left, (off_t)window_bytes);
        size_t page = sysconf(_SC_PAGE_SIZE);
        map_offset = (pos / page) * page;
        off_t offset_diff = pos - map_offset;
        map_len = page_align(req + offset_diff);
        map_base = mmap(nullptr, map_len, PROT_READ, MAP_SHARED, fd, map_offset);
        if (map_base == MAP_FAILED) { 
            return false; 
        }
        arr = (u64*)((char*)map_base + offset_diff);
        u64_count = req / sizeof(u64);
        idx = 0;
        return u64_count > 0;
    }
    bool has_next() {
        if (map_base == MAP_FAILED) {
            return false;
        }
        if (idx < u64_count) {
            return true;
        }
        pos += u64_count * sizeof(u64);
        if (pos >= file_size) {
            return false;
        }
        return map_next();
    }
    bool peek(u64 &out) {
        if (!has_next()) {
            return false;
        }
        out = arr[idx];
        return true;
    }
    bool pop(u64 &out) {
        if (!has_next()) {
            return false;
        }
        out = arr[idx++];
        return true;
    }
};

bool write_all(int fd, const void* buf, size_t bytes) {
    const char* p = (const char*)buf;
    size_t left = bytes;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("write");
            return false;
        }
        left -= (size_t)w;
        p += w;
    }
    return true;
}

bool create_initial_runs(const string &input_path, size_t mapping_limit, vector<string> &runs) {
    int infd = open(input_path.c_str(), O_RDONLY);
    if (infd < 0) { 
        perror("open input"); 
        return false; 
    }
    off_t fsize = get_file_size(infd);
    if (fsize < 0) { 
        perror("stat"); 
        close(infd); 
        return false; 
    }
    size_t total_u64 = fsize / sizeof(u64);
    size_t run_bytes = mapping_limit; // bytes per run mapped and sorted
    if (run_bytes < sizeof(u64)) {
        run_bytes = sizeof(u64);
    }

    off_t offset = 0;
    size_t page = sysconf(_SC_PAGE_SIZE);
    while (offset < fsize) {
        off_t bytes_left = fsize - offset;
        size_t to_map = (size_t)min<off_t>(bytes_left, (off_t)run_bytes);
        size_t page_aligned = page_align(to_map);
        off_t map_offset = (offset / page) * page;
        off_t offset_diff = offset - map_offset;
        size_t map_len = page_align(to_map + offset_diff);
        void* map_ptr = mmap(nullptr, map_len, PROT_READ, MAP_SHARED, infd, map_offset);
        if (map_ptr == MAP_FAILED) { 
            perror("mmap"); 
            close(infd); 
            return false; 
        }
        u64* arr = (u64*)((char*)map_ptr + offset_diff);
        size_t u64_count = to_map / sizeof(u64);
        vector<u64> buf;
        buf.reserve(u64_count);
        for (size_t i = 0; i < u64_count; ++i) {
            buf.push_back(arr[i]);
        }
        munmap(map_ptr, map_len);

        sort(buf.begin(), buf.end());

        string runname = make_temp_filename();
        int outfd = open(runname.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (outfd < 0) { 
            perror("open run"); 
            close(infd); 
            return false; 
        }
        if (!write_all(outfd, buf.data(), buf.size()*sizeof(u64))) { 
            close(outfd); 
            close(infd); 
            return false; 
        }
        close(outfd);
        runs.push_back(runname);

        offset += to_map;
    }
    close(infd);
    return true;
}

bool merge_two_runs(const string &a, const string &b, const string &outname, size_t mapping_limit) {
    size_t half = max<size_t>(mapping_limit/2, 4096);
    SimpleMappedReader A, B;
    
    int outfd = open(outname.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (outfd < 0) { 
        perror("open out"); 
        return false; 
    }
    const size_t OUT_BUF_U64 = 1<<16; // 65536 *8 = 512KB
    vector<u64> outbuf;
    outbuf.reserve(OUT_BUF_U64);

    bool a_has = A.has_next();
    bool b_has = B.has_next();
    u64 aval=0, bval=0;
    if (a_has) {
        A.peek(aval);
    }
    if (b_has) {
        B.peek(bval);
    }

    while (a_has && b_has) {
        if (aval <= bval) {
            A.pop(aval);
            outbuf.push_back(aval);
            a_has = A.has_next();
            if (a_has) {
                A.peek(aval);
            }
        } else {
            B.pop(bval);
            outbuf.push_back(bval);
            b_has = B.has_next();
            if (b_has) {
                B.peek(bval);
            }
        }
        if (outbuf.size() >= OUT_BUF_U64) {
            if (!write_all(outfd, outbuf.data(), outbuf.size()*sizeof(u64))) { 
                close(outfd); 
                return false; 
            }
            outbuf.clear();
        }
    }
    // drain remaining
    while (a_has) {
        A.pop(aval);
        outbuf.push_back(aval);
        a_has = A.has_next();
        if (outbuf.size() >= OUT_BUF_U64) {
            if (!write_all(outfd, outbuf.data(), outbuf.size()*sizeof(u64))) { 
                close(outfd); 
                return false; 
            }
            outbuf.clear();
        }
    }
    while (b_has) {
        B.pop(bval);
        outbuf.push_back(bval);
        b_has = B.has_next();
        if (outbuf.size() >= OUT_BUF_U64) {
            if (!write_all(outfd, outbuf.data(), outbuf.size()*sizeof(u64))) { 
                close(outfd); 
                return false; 
            }
            outbuf.clear();
        }
    }
    if (!outbuf.empty()) {
        if (!write_all(outfd, outbuf.data(), outbuf.size()*sizeof(u64))) { 
            close(outfd); 
            return false; 
        }
    }
    close(outfd);
    A.close_fd(); B.close_fd();
    return true;
}

bool external_sort(const string &input_path, size_t mapping_limit_bytes) {
    vector<string> runs;
    fprintf(stderr, "Creating initial runs (mapping limit %zu bytes)...\n", mapping_limit_bytes);
    if (!create_initial_runs(input_path, mapping_limit_bytes, runs)) {
        return false;
    }
    fprintf(stderr, "Created %zu runs\n", runs.size());
    if (runs.empty()) {
        fprintf(stderr, "No data to sort.\n"); return true;
    }
    // merge pairwise until single run
    size_t pass = 0;
    while (runs.size() > 1) {
        vector<string> next;
        for (size_t i = 0; i < runs.size(); i += 2) {
            if (i + 1 >= runs.size()) {
                next.push_back(runs[i]);
            } else {
                string outname = make_temp_filename();
                fprintf(stderr, "Merging %s + %s -> %s\n", runs[i].c_str(), runs[i+1].c_str(), outname.c_str());
                if (!merge_two_runs(runs[i], runs[i+1], outname, mapping_limit_bytes)) {
                    return false;
                }
                // remove input runs
                unlink(runs[i].c_str());
                unlink(runs[i+1].c_str());
                next.push_back(outname);
            }
        }
        runs.swap(next);
        pass++;
        fprintf(stderr, "After pass %zu: %zu runs\n", pass, runs.size());
    }
    string outpath = input_path + string(".sorted");
    fs::rename(runs[0], outpath);
    fprintf(stderr, "Sorted output written to %s\n", outpath.c_str());
    return true;
}

bool generate_file(const string &path, size_t count, unsigned seed) {
    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) { 
        perror("open generate"); 
        return false; 
    }
    std::mt19937_64 rng(seed);
    const size_t BUF = 1<<16; // 65536
    vector<u64> buf;
    buf.reserve(BUF);
    size_t written = 0;
    while (written < count) {
        buf.clear();
        size_t to = min<size_t>(BUF, count - written);
        for (size_t i = 0; i < to; ++i) {
            buf.push_back(rng());
        }
        if (!write_all(fd, buf.data(), buf.size()*sizeof(u64))) { 
            close(fd); 
            return false; 
        }
        written += to;
    }
    close(fd);
    return true;
}

bool check_sorted(const string &path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) { 
        perror("open check"); 
        return false; 
    }
    off_t fsize = get_file_size(fd);
    if (fsize < 0) { 
        perror("stat"); 
        close(fd); 
        return false; 
    }
    size_t total = fsize / sizeof(u64);
    const size_t MAP_BYTES = 1<<20;
    off_t offset = 0;
    u64 prev = 0;
    bool first = true;
    while (offset < fsize) {
        off_t bytes_left = fsize - offset;
        size_t to_map = (size_t)min<off_t>(bytes_left, (off_t)MAP_BYTES);
        size_t page = sysconf(_SC_PAGE_SIZE);
        off_t map_offset = (offset / page) * page;
        off_t offset_diff = offset - map_offset;
        size_t map_len = page_align(to_map + offset_diff);
        void* ptr = mmap(nullptr, map_len, PROT_READ, MAP_SHARED, fd, map_offset);
        if (ptr == MAP_FAILED) { 
            perror("mmap"); 
            close(fd); 
            return false; 
        }
        u64* arr = (u64*)((char*)ptr + offset_diff);
        size_t cnt = to_map / sizeof(u64);
        for (size_t i = 0; i < cnt; ++i) {
            u64 v = arr[i];
            if (!first && v < prev) {
                fprintf(stderr, "Not sorted at offset %jd: %" PRIu64 " < %" PRIu64 "\n", (intmax_t)(offset + i*sizeof(u64)), v, prev);
                munmap(ptr, map_len);
                close(fd);
                return false;
            }
            prev = v;
            first = false;
        }
        munmap(ptr, map_len);
        offset += to_map;
    }
    close(fd);
    fprintf(stderr, "File appears sorted (%zu elements).\n", total);
    return true;
}

int main(int argc, char** argv) {
    string cmd = argv[1];
    if (cmd == "generate") {
        string path = argv[2];
        size_t count = stoull(argv[3]);
        unsigned seed = (argc >=5) ? (unsigned)stoi(argv[4]) : (unsigned)time(nullptr);
        fprintf(stderr, "Generated %zu numbers to %s\n", count, path.c_str());
        return 0;
    } else if (cmd == "sort") {
        string path = argv[2];
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) { 
            perror("open"); 
            return 1; 
        }
        off_t fsize = get_file_size(fd);
        if (fsize < 0) { 
            perror("stat"); 
            close(fd); 
            return 1; 
        }
        close(fd);
        size_t mapping_limit = 0;
        if (argc >= 4) {
            mapping_limit = (size_t)stoull(argv[3]) * 1024 * 1024;
        } else {
            mapping_limit = (size_t)fsize / 10;
        }
        if (mapping_limit < 1<<20) {
            mapping_limit = 1<<20;
        }
        if (mapping_limit > (size_t)fsize) {
            mapping_limit = (size_t)fsize;
        }
        if (!external_sort(path, mapping_limit)){
             return 1;
        }
        return 0;
    } else if (cmd == "check") {
        string path = argv[2];
        if (!check_sorted(path)) return 1;
        return 0;
    }
    return 1;  
}
