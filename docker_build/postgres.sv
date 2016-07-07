[program:postgresql]
command=su -c "pg_ctl start -D data" - postgres
