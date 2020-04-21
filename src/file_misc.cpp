#include "file_misc.h"

#include <sys/ioctl.h>
#include <net/if.h>
#include <fcntl.h>
#include <stdexcept>
#include <string.h>
#include <sys/epoll.h>
#include "network.h"

using namespace shim;

int bionic::to_host_file_status_flags(bionic::file_status_flags flags) {
    using flag = file_status_flags;
    if (((uint32_t) flags & (~(uint32_t) flag::KNOWN_FLAGS)) != 0)
        throw std::runtime_error("Unsupported fd flag used");

    int ret = 0;
    if ((uint32_t) flags & (uint32_t) flag::RDONLY) ret |= O_RDONLY;
    if ((uint32_t) flags & (uint32_t) flag::WRONLY) ret |= O_WRONLY;
    if ((uint32_t) flags & (uint32_t) flag::RDWR) ret |= O_RDWR;
    if ((uint32_t) flags & (uint32_t) flag::CREAT) ret |= O_CREAT;
    if ((uint32_t) flags & (uint32_t) flag::EXCL) ret |= O_EXCL;
    if ((uint32_t) flags & (uint32_t) flag::NOCTTY) ret |= O_NOCTTY;
    if ((uint32_t) flags & (uint32_t) flag::TRUNC) ret |= O_TRUNC;
    if ((uint32_t) flags & (uint32_t) flag::APPEND) ret |= O_APPEND;
    if ((uint32_t) flags & (uint32_t) flag::NONBLOCK) ret |= O_NONBLOCK;
    return ret;
}

int shim::ioctl(int fd, bionic::ioctl_index cmd, void *arg) {
    switch (cmd) {
        case bionic::ioctl_index::FILE_NBIO:
            return ::ioctl(fd, FIONBIO, arg);
        case bionic::ioctl_index::SOCKET_CGIFCONF: {
            auto buf = (bionic::ifconf *) arg;
            size_t cnt = buf->len / sizeof(bionic::ifreq);
            ifconf *hbuf = new ifconf[cnt * sizeof(::ifreq)];
            int ret = ::ioctl(fd, SIOCGIFCONF, hbuf);
            if (ret < 0)
                return ret;
            cnt = hbuf->ifc_len / sizeof(::ifreq);
            buf->len = cnt * sizeof(bionic::ifreq);
            for (size_t i = 0; i < cnt; i++) {
                strncpy(buf->req->name, hbuf->ifc_req[i].ifr_name, 16);
                hbuf->ifc_req[i].ifr_name[15] = 0;

                bionic::from_host(&hbuf->ifc_req[i].ifr_addr, &buf->req->addr);
            }
            delete[] hbuf;
            return ret;
        }
        default:
            throw std::runtime_error("Unsupported ioctl");
    }
}

int shim::open(const char *pathname, bionic::file_status_flags flags, ...) {
    va_list ap;
    mode_t mode = 0;

    int hflags = bionic::to_host_file_status_flags(flags);
    if (hflags & O_CREAT) {
        va_start(ap, flags);
        mode = (mode_t) va_arg(ap, int);
        va_end(ap);
    }

    return ::open(pathname, hflags, mode);
}

int shim::fcntl(int fd, bionic::fcntl_index cmd, void *arg) {
    switch (cmd) {
        case bionic::fcntl_index::SETFD:
            return ::fcntl(fd, F_SETFD, (int)(intptr_t) arg);
        case bionic::fcntl_index::SETFL:
            return ::fcntl(fd, F_SETFL, to_host_file_status_flags((bionic::file_status_flags) (size_t) arg));
        case bionic::fcntl_index::SETLK: {
            auto afl = (bionic::flock *) arg;
            flock fl {};
            fl.l_type = afl->l_type;
            fl.l_whence = afl->l_whence;
            fl.l_start = afl->l_start;
            fl.l_len = afl->l_len;
            fl.l_pid = afl->l_pid;
            return ::fcntl(fd, F_SETLK, &fl);
        }
        default:
            throw std::runtime_error("Unsupported fcntl");
    }
}

int shim::poll_via_select(struct pollfd *fds, nfds_t nfds, int timeout) {
    // Mac OS has a broken poll implementation
    struct timeval t;
    t.tv_sec = timeout / 1000;
    t.tv_usec = (timeout % 1000) * 1000;

    fd_set r_fdset, w_fdset, e_fdset;
    int maxfd = 0;
    FD_ZERO(&r_fdset);
    FD_ZERO(&w_fdset);
    FD_ZERO(&e_fdset);
    for (nfds_t i = 0; i < nfds; i++) {
        if (fds[i].fd > maxfd)
            maxfd = fds[i].fd;
        if (fds[i].events & POLLIN || fds[i].events & POLLPRI)
            FD_SET(fds[i].fd, &r_fdset);
        if (fds[i].events & POLLOUT)
            FD_SET(fds[i].fd, &w_fdset);
        FD_SET(fds[i].fd, &e_fdset);
    }
    int ret = select(maxfd + 1, &r_fdset, &w_fdset, &e_fdset, &t);
    for (nfds_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (FD_ISSET(fds[i].fd, &r_fdset))
            fds[i].revents |= POLLIN;
        if (FD_ISSET(fds[i].fd, &w_fdset))
            fds[i].revents |= POLLOUT;
        if (FD_ISSET(fds[i].fd, &e_fdset))
            fds[i].revents |= POLLERR;
    }
    return ret;
}

void shim::add_ioctl_shimmed_symbols(std::vector<shim::shimmed_symbol> &list) {
    list.push_back({"ioctl", ioctl});
}

void shim::add_fcntl_shimmed_symbols(std::vector<shim::shimmed_symbol> &list) {
    list.insert(list.end(), {
        {"open", open},
        {"fcntl", fcntl},
    });
}

void shim::add_poll_select_shimmed_symbols(std::vector<shim::shimmed_symbol> &list) {
    list.insert(list.end(), {
#ifdef __APPLE__
        {"poll", poll_via_select},
#else
        {"poll", ::poll},
#endif
        {"select", ::select},
    });
}

void shim::add_epoll_shimmed_symbols(std::vector<shim::shimmed_symbol> &list) {
    list.insert(list.end(), {
        /* sys/epoll.h */
        {"epoll_create", epoll_create},
        {"epoll_create1", epoll_create1},
        {"epoll_ctl", epoll_ctl},
        {"epoll_wait", epoll_wait},
    });
}