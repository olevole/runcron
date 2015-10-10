# runcron
cron wrapper to guarantee of a single process at time:

 * create a argv-based lock file in /tmp/*.flock
 * show summary via syslog and email

usage:

  ./runcron [-m mailto] <cmd> ...
