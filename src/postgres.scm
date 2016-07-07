; postgres helpers
(define-module (gusher postgres)
	#:use-module (guile-user)
	#:use-module (gusher misc)
	#:export
		(pg-query pg-exec pg-one-row pg-open configure-database))

(define (pg-query dbh query args)
	; safely plug SQL query parameters into query template,
	; to be used with postgres calls below
	(apply format
		(append (list #f query)
			(map (lambda (item) (pg-format dbh item)) args))))
(define (pg-exec dbh query . args)
	(pg-exec-primitive dbh
		(if (null? args) query (pg-query dbh query args))))
(define (pg-one-row dbh query . args)
	(pg-one-row-primitive dbh
		(if (null? args) query (pg-query dbh query args))))

; database configuration
(define *database-connection-profile* "")
(define* (pg-open #:optional (profile *database-connection-profile*))
	(pg-open-primitive profile))
(define configure-database
	(let ([query
				(deflate "create table if not exists _kv_ (
					namespace varchar,
					key varchar,
					value text
					);
				create index on _kv_ (namespace, key);")])
		(lambda (profile)
			(set! *database-connection-profile* profile)
			(let ([dbh (pg-open)])
				(pg-exec dbh query)
				(pg-close dbh)))))
