# runcron
# Author olevole@olevole.ru

cron wrapper to guarantee of a single process at time with extra feature:

 * create a argv-based lock file in /tmp/*.flock
 * show summary via syslog and email
 * post-execution action

usage:

  ./runcron [-m mailto] <cmd> ...
