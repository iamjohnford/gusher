;; SQL-hosted key-value store

(define-module (gusher kv)
	#:use-module (guile-user)
	#:use-module (gusher misc)
	#:use-module (gusher postgres)
	#:export
		(kv-open kv-close kv-get kv-set kv-exists kv-count kv-keys kv-del))

(define (kv-open namespace . other) (cons (pg-open) namespace))
(define (kv-close kv-handle) (pg-close (car kv-handle)))
(define kv-get
	(let ([query
				(deflate "select value
					from _kv_
					where (namespace=~a)
					and (key=~a)")])
		(lambda (kv-handle key)
			(let ([row (pg-one-row (car kv-handle) query
							(cdr kv-handle) key)])
				(pg-cell row 'value)))))
(define kv-set
	(let ([update-query
				(deflate "update _kv_ set
					value=~a
					where (namespace=~a)
					and (key=~a)")]
			[insert-query
				(deflate "insert into _kv_ (namespace, key, value)
					values (~a, ~a, ~a)")])
		(lambda (kv-handle key value)
			(let ([res
						(pg-exec (car kv-handle) update-query
							value (cdr kv-handle) key)])
				(when (< (pg-cmd-tuples res) 1)
					(pg-exec (car kv-handle) insert-query
						(cdr kv-handle) key value))))))
(define kv-exists
	(let ([query
			(deflate "select key
					from _kv_
					where (namespace=~a)
					and (key=~a)")])
		(lambda (kv-handle key)
			(let ([row (pg-one-row (car kv-handle) query
							(cdr kv-handle) key)])
				(not (not row))))))
(define kv-count
	(let ([query
				(deflate "select count(*) as n
					from _kv_
					where (namespace=~a)")])
		(lambda (kv-handle)
			(let ([row (pg-one-row (car kv-handle) query
							(cdr kv-handle))])
				(pg-cell row 'n)))))
(define kv-keys
	(let ([query "select key from _kv_ where (namespace=~a)"])
		(lambda (kv-handle)
			(let ([res (pg-exec (car kv-handle) query (cdr kv-handle))])
				(pg-map-rows res (lambda (row) (pg-cell row 'key)))))))
(define kv-del
	(let ([query "delete from _kv_ where (namespace=~a) and (key=~a)"])
		(lambda (kv-handle key)
			(pg-exec (car kv-handle) query
				(cdr kv-handle) key))))
