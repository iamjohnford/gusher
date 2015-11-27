; postgres helpers
(define-module (gusher postgres)
	#:use-module (guile-user)
	#:export
		(pg-query pg-query-exec pg-query-one-row pg-open
			configure-database))

(define (pg-query dbh query args)
	; safely plug SQL query parameters into query template,
	; to be used with postgres calls below
	(apply format
		(append (list #f query)
			(map (lambda (item) (pg-format dbh item)) args))))
(define (pg-query-exec dbh query . args)
	(pg-exec dbh (pg-query dbh query args)))
(define (pg-query-one-row dbh query . args)
	(pg-one-row dbh (pg-query dbh query args)))

; database configuration
(define *database-connection-profile* "")
(define* (pg-open #:optional (profile *database-connection-profile*))
	(pg-open-primitive profile))
(define assure-key-value-store-query "\
create table _kv_ (
	namespace varchar,
	key varchar,
	value text
	);
create index on _kv_ (namespace, key);")
(define (configure-database profile)
	(set! *database-connection-profile* profile)
	(let ([dbh (pg-open)])
		(pg-exec dbh assure-key-value-store-query)
		(pg-close dbh)))
