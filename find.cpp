#include <queue>
#include <iostream>
#include <string>
#include <cmath>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>

static const unsigned long BUF_SIZE = 4096;

void substr(const std::string &str, char *arg, size_t from, size_t to) {
    arg = new char[to - from + 1];
    size_t index = 0;
    for (size_t i = from; i < to; ++i) {
        arg[index++] = str[i];
    }
    arg[index] = '\0';
}

int str_to_int(const std::string &str, size_t from, size_t to) {
    int res = 0;
    for (size_t i = from; i < to; ++i) {
        if (str[i] < '0' || str[i] > '9')
            return -1;
        res += (str[i] - '0') * pow(10, str.size() - i - 1 - from);
    }
    return res;
}

void get_first_arg(const std::string & path, char *arg_first) {
  size_t index = path.size() - 1;
  for (; index != 0 && path[index] != '/'; --index) {}
  substr(path, arg_first, !index ? 0 : index + 1, path.size());
}

struct linux_dirent {
    long d_ino;
    off_t d_off;
    unsigned short d_reclen;
    char d_name[];
};

class flags_wrapper {
public:
    bool isExec() const {
        return EXEC;
    }

    bool satisfied(long inode, const char *name, off_t reclen, nlink_t nlink) const {
        bool result = true;
        if (INUM) {
            if (inum != inode ) return false;
        }
        if (NAME) {
            size_t i = 0;
            while (this->name_[i] != '\0' && name[i] != '\0') {
                if (this->name_[i] != name[i])
                    return false;
                   ++i;
            }
            result &= (this->name_[i] == '\0' && name[i] == '\0');
        }

        if (NLINKS) {
            if (this->nlinks != nlink) return false;
        }
        if (SIZE) {
            switch (size_sign) {
            case '-' :{
                result &= (reclen <= size_);
                break;
            }
            case'+': {
                result &= (reclen >= size_);
                break;
            }
            case '=': {
                result &= (reclen == size_);
                break;
            }
            default: {
                //error, it never happens. NEVER.
                return false;
            }
            }
        }
        return result;
    }
    const char * get_exe_path() const {
        return path_to_exec;
    }

    int set_flags(int argc, char* argv[]) {
        for (size_t i = 2; i < static_cast<size_t>(argc); i += 2) {
            size_t arg_name_hash = std::hash<std::string>{}(std::string(argv[i]));
            if (argv[i +1] == nullptr)
                return -1;
            std::string arg(argv[i + 1]);
            if (arg_name_hash == flags_hashes[0]) {
                INUM = true;
                inum = str_to_int(arg, 0, arg.size());
                if (inum == -1)
                    return -1;
            } else if (arg_name_hash == flags_hashes[1]) {
                NAME = true;
                name_ = argv[i + 1];
            } else if (arg_name_hash == flags_hashes[2]) {
                SIZE = true;
                size_sign = arg[0];
                if (size_sign != '-' && size_sign != '+' && size_sign != '=')
                    return -1;

                if ((arg.back() >= '0' && arg.back() <= '9')) {
                    //512-byte blocks by default
                    size_ = str_to_int(arg, 1, arg.size()) * (1 << 9);
                } else {
                    int size = str_to_int(arg, 1, arg.size() - 1);
                    switch(arg.back()) {
                    case 'M': {
                        size_ = size * (1 << 20);
                        break;
                    }
                    case 'K': {
                        size_ = size * (1 << 10);
                        break;
                    }
                    case 'G' :{
                        size_ = size * (1 << 30);
                        break;
                    }
                    case 'b':{
                        size_ = size * (1 << 9);
                        break;
                    }
                    case 'c': {
                        size_ = size;
                        break;
                    }
                    default: {
                        std::cerr << "Invalid size. Usage: -size n[ckGM]\n"
                                                    "File uses n units of space, rounding up.  The following suffixes can be used:\n"
                                                    "`b'    for 512-byte blocks (this is the default if no suffix is used)\n"
                                                    "`c'    for bytes\n"
                                                    "`K'    for Kibibytes (KiB, units of 1024 bytes)\n"
                                                   " `M'    for Mebibytes (MiB, units of 1024 * 1024 = 1048576 bytes)\n"
                                                   " `G'    for Gibibytes (GiB, units of 1024 * 1024 * 1024 = 1073741824 bytes)\n\n";
                        return -1;
                    }
                    }
                }
                if (size_ < 0)
                    return -1;

            } else if (arg_name_hash == flags_hashes[3]) {
                NLINKS = true;
                int tmp  = str_to_int(arg, 0, arg.size());
                if (tmp == -1) return -1;
                nlinks = static_cast<nlink_t>(tmp);
            } else if (arg_name_hash == flags_hashes[4]) {
                EXEC = true;
                path_to_exec = argv[i + 1];
            } else {
                return -1;
            }
        }
        return 0;
    }
private:
    char size_sign = '\0';
    bool INUM = false, SIZE = false, NAME = false, NLINKS = false, EXEC = false;
    long inum = 0;
    nlink_t nlinks = 0;
    off_t size_ = 0;
    char *name_ = nullptr, *path_to_exec = nullptr;
    const size_t flags_hashes[5] = {
        std::hash<std::string>{}(std::string("-inum")),
        std::hash<std::string>{}(std::string("-name")),
        std::hash<std::string>{}(std::string("-size")),
        std::hash<std::string>{}(std::string("-nlinks")),
        std::hash<std::string>{}(std::string("-exec"))
    };
};

void clean(char **str, size_t size) {
    for (size_t i = 0; i <= size; ++i) {
        if (str[i] != nullptr)
            delete [] str[i];
    }
    delete [] str;
}

int execute_with_argv(const flags_wrapper &flags_wrp, const std::vector<std::string> &args_str) {
    char **arguments = new char *[args_str.size() + 2];

    get_first_arg(flags_wrp.get_exe_path(), arguments[0]);

    for (size_t i = 0; i < args_str.size(); ++i) {
        arguments[1 + i] = new char[args_str[i].size() + 1];
        strcpy(arguments[1 + i], args_str[i].c_str());
        arguments[1 + i][args_str[i].size()] = '\0';
    }

    arguments[args_str.size() + 1] = nullptr;

    pid_t pid = fork();

    if (pid == 0) {
        int prog_stat = execvp(flags_wrp.get_exe_path(), arguments);
        std::cout << "Can't execute " << flags_wrp.get_exe_path() << ". " << strerror(errno) << "\n";
        return prog_stat;
    } else if (pid == -1) {
        std::cerr << "Can't create new process. " << strerror(errno) << "\n";
        clean(arguments, args_str.size() + 2);
        return -1;
    } else {
        int wstatus;
        if (waitpid(pid, &wstatus, WUNTRACED) == -1) {
            std::cerr << "Error during execution program. " << strerror(errno) << '\n';
        } else {
            if (WIFEXITED(wstatus)) {
                std::cout << flags_wrp.get_exe_path() <<  ": exit code is " << WEXITSTATUS(wstatus) << "\n" ;
            } else {
                std::cerr << "Error during termination of " << flags_wrp.get_exe_path() << " execution.\n";
            }
        }
        clean(arguments, args_str.size() + 2);
        return 0;
    }
}

int run(const char *path, const flags_wrapper &flags_wrp){
    const char *this_dir = ".", *this_parent_dir = "..";
    bool exec = flags_wrp.isExec();
    std::queue<std::string> q;
    q.push(std::string(path));

    struct stat statbuf;

    std::vector<std::string> argv;

    char buffer[BUF_SIZE];

    while (!q.empty()) {
        std::string cur_path = std::move(q.front());
        q.pop();
        int cur_fd = open(cur_path.c_str(), O_RDONLY, O_DIRECTORY);

        if (cur_fd == -1) {
             std::cerr << "Can't open directory " << cur_path << ". \n" << strerror(errno) << "\n";
        } else {
            struct linux_dirent *dirent;
            while (true) {
                int nread = syscall(SYS_getdents, cur_fd, buffer, BUF_SIZE);
                if (nread == -1) {
                    std::cerr << "Can't get entry of directory " << cur_path << ". " << strerror(errno) << "\n";
                    break;
                }
                if (!nread) break;
                char d_type;

                for (int bpos = 0; bpos < nread;) {
                    dirent = reinterpret_cast<struct linux_dirent *>(buffer + bpos);

                    std::string child_path(cur_path);
                    child_path.append("/");
                    child_path.append(dirent->d_name);

                    d_type = *(buffer + bpos + dirent->d_reclen - 1);

                    //If symlink links to directory, "find" doesn't go to this directory
                    if (d_type == DT_DIR && strcmp(this_parent_dir, dirent->d_name) && strcmp(this_dir, dirent->d_name)) {
                        q.push(child_path);
                    }

                    //For symlink "find" will get inode for this symlink, but not for file this symlink links to.
                    int stat_status = stat(child_path.c_str(), &statbuf);
                    if (stat_status == -1) {
                        std::cerr << "Can't get information about file: " << child_path << ". " << strerror(errno) << "\n";
                    } else {
                        if (flags_wrp.satisfied(dirent->d_ino, dirent->d_name, statbuf.st_size, statbuf.st_nlink) && strcmp(this_parent_dir, dirent->d_name) && strcmp(this_dir, dirent->d_name)) {
                            if (!exec) {
                                std::cout << child_path << "\n";
                            } else {
                                argv.push_back(child_path);
                            }
                        }
                    }
                    bpos += dirent->d_reclen;
                }
            }
            if (close(cur_fd) == -1) {
                std::cerr << cur_path << ": " << strerror(errno) << "\n";
            }
        }
    }

    if (exec) {
        return execute_with_argv(flags_wrp, argv);
    }

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Incorrect input. Usage: path_to_executable_file [args...]\n"
                     "Supported arguments: [-name name] [-size [+=-]size[cbMKG]] [-inum inode] [-exec path_to_exe_file] [-nlinks nlinks]\n";
        return 0;
    }
    flags_wrapper flags;
    char *path = argv[1];

    int status_flag = flags.set_flags(argc, argv);

    if (status_flag == -1) {
        std::cerr << "Wrong arguments. Supported arguments: [-name name] [-size [+=-]size[cbMKG] [-inum inode] [-exec path_to_exe_file] [-nlinks nlinks]";
        return 0;
    }

    return run( path, flags);
}

