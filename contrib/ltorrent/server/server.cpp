/*
* Written by Dmitry Chirikov <dmitry@chirikov.ru>
* This file is part of Luna, cluster provisioning tool
* https://github.com/dchirikov/luna
*
* This file is part of Luna.
*
* Luna is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.

* Luna is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with Luna.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "server.hpp"

bool LTorrent::running_ = true;

LTorrent::LTorrent(const OptionParser &opts)
  : opts_(opts),
    logger_(log4cplus::Logger::getInstance(DEFAULT_LOGGER_NAME))
{
  log_trace(__PRETTY_FUNCTION__);
};

int create_dir(std::string path) {
  struct stat info;
  if (stat(path.c_str(), &info) == 0 ) {
    if ((info.st_mode & S_IFMT) == S_IFDIR) {
      return(EXIT_SUCCESS);
    } else {
      // logger is unavailable here yet, so use stderr
      std::cerr << "'" << path << "' exists and not directory\n";
      return(EXIT_FAILURE);
    }
  }
  if (mkdir(path.c_str(), 0750)) {
    std::perror(path.c_str());
    std::cerr << "Unable to create dir '" << path << "'\n";
    return(EXIT_FAILURE);
  }
  return(EXIT_SUCCESS);
}

int chowner(std::string path, uid_t pw_uid, gid_t pw_gid) {
  if (chown(path.c_str(), pw_uid, pw_gid)) {
    std::perror(path.c_str());
    std::cerr << "Unable to chown "
      << pw_uid << ":" << pw_gid << " " << path << "\n";
    return(EXIT_FAILURE);
  }
  return(EXIT_SUCCESS);
}

int create_dir_and_chown(std::string path, uid_t pw_uid, gid_t pw_gid) {
  if (create_dir(path)) {
    return(EXIT_FAILURE);
  }
  if (chowner(path, pw_uid, pw_gid)) {
    return(EXIT_FAILURE);
  }
  return(EXIT_SUCCESS);
}

int LTorrent::createDirs(const OptionParser &opts) {
  if (create_dir_and_chown(opts.logDir, opts.pw_uid, opts.pw_gid)) {
    return(EXIT_FAILURE);
  }
  if (create_dir_and_chown(opts.pidDir, opts.pw_uid, opts.pw_gid)) {
    return(EXIT_FAILURE);
  }
  return(EXIT_SUCCESS);
}

int LTorrent::killProcess(const OptionParser &opts) {
  // check if pid file exists
  struct stat info;
  if (stat(opts.pidFile.c_str(), &info) != 0 ) {
    std::perror("Error accessing PID file.");
    std::cerr << "Unable to find '" << opts.pidFile << "'. "
      << "Is daemon running?\n";
    return(EXIT_FAILURE);
  }
  // check if piddile is a file, actually
  if (!((info.st_mode & S_IFMT) == S_IFREG)) {
    std::cerr << "'" << opts.pidFile << "' is not a regular file\n";
    return(EXIT_FAILURE);
  }
  // read pid from pidfile
  std::ifstream pidfile(opts.pidFile);
  pid_t pid;
  pidfile >> pid;
  // check if process exists
  if (kill(pid, 0)) {
    std::cerr << "Process with PID '" << pid << "' is not running\n";
    return(EXIT_FAILURE);
  }
  // get process group for PID
  pid_t pgid;
  pgid = getpgid(pid);

  int seconds = 0;
  while (seconds < opts.killtimeout) {
    sleep(1);
    if (killpg(pgid, SIGTERM)) {
      return(EXIT_SUCCESS);
    }
    seconds++;
  }
  std::cerr << "Timeout killing process. Sending SIGKILL.\n";
  killpg(pgid, SIGKILL);
  // do cleanup for killed process
  if (std::remove(opts.pidFile.c_str()) != 0 ) {
    std::cerr << "Unable to delete '" << opts.pidFile << "'\n";
    return(EXIT_FAILURE);
  }
  return(EXIT_SUCCESS);
}

void LTorrent::stopHandler(int signal) {
  auto logger = log4cplus::Logger::getInstance(DEFAULT_LOGGER_NAME);
  LOG4CPLUS_TRACE(logger, __PRETTY_FUNCTION__);
  running_ = false;
}

int LTorrent::changeUser(const OptionParser &opts) {
  if (setuid(opts.pw_uid)) {
    std::perror("Unable to change user");
    std::cerr << "Unable to change EUID to " << opts.pw_uid
      << " to run as " << opts.user <<  "\n";
    return(EXIT_FAILURE);
  }
  return(EXIT_SUCCESS);
}

int LTorrent::daemonize() {
  log_trace(__PRETTY_FUNCTION__);
  if (!opts_.daemonize) {
    log_debug("Running in foreground");
    return(EXIT_SUCCESS);
  }
  log_debug("Starting to daemonize");
  pid_t pid;
  // first fork
  pid = fork();
  if (pid < 0) {
    log_error("Unable to perform first fork");
    return(EXIT_FAILURE);
  }
  // parent exit
  if (pid > 0) {
    exit(EXIT_SUCCESS);
  }
  log_debug("First fork succeded");
  // child become session leader
  if (setsid() < 0) {
    log_error("Unable to become session leader.");
    exit(EXIT_FAILURE);
  }
  // second fork
  if (pid < 0) {
    log_error("Unable to perform second fork");
    return(EXIT_FAILURE);
  }
  // parent exit (first child)
  if (pid > 0) {
    exit(EXIT_SUCCESS);
  }
  // current process is grandchild
  // set new file perms
  umask(0);
  // close STDIN, SDTOUT and STDERR
  for (int i=0; i>=2; i++)
  {
    close(i);
  }

  return(EXIT_SUCCESS);
}

int LTorrent::registerHandlers() {
  log_trace(__PRETTY_FUNCTION__);
  std::signal(SIGINT, stopHandler);
  std::signal(SIGTERM, stopHandler);
  return(EXIT_SUCCESS);
}

int LTorrent::createPidFile() {
  log_trace(__PRETTY_FUNCTION__);
  struct stat info;
  if (stat(opts_.pidFile.c_str(), &info) == 0 ) {
    log_error("'" << opts_.pidFile << "' already exists.");
    return(EXIT_FAILURE);
  }
  auto pid = getpid();
  log_info("Running PID " << pid);
  std::ofstream pidfile;
  log_debug("Write PID to pidfile");
  pidfile.open(opts_.pidFile);
  pidfile << pid;
  pidfile << "\n";
  pidfile.close();
  return(EXIT_SUCCESS);
}

int LTorrent::cleanup() {
  log_trace(__PRETTY_FUNCTION__);
  if (std::remove(opts_.pidFile.c_str()) != 0 ) {
    log_error("Unable to delete " << opts_.pidFile);
    return(EXIT_FAILURE);
  }
  return(EXIT_SUCCESS);
}

int LTorrent::run() {
  log_trace(__PRETTY_FUNCTION__);
  // change directory
  if (chdir(opts_.homeDir.c_str())) {
    log_error("Unable to change directory to '" << opts_.homeDir << "'");
    return(EXIT_FAILURE);
  }
  log_debug("Run main loop");
  while (running_) {
    log_trace("running");
    sleep(1);
  }
  log_debug("stopping");

  return(0);
}

