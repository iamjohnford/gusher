;; SQL-hosted key-value store

(define-module (gusher kv)
	#:use-module (guile-user)
	#:use-module (gusher postgres)
	#:export
		(kv-open kv-close kv-get kv-set kv-exists kv-count kv-keys kv-del))

(define (kv-open namespace read-only) (cons (pg-open) namespace))
(define (kv-close kv-handle) (pg-close (car kv-handle)))
(define kv-get-query "\
select value
	from _kv_
	where (namespace=~a)
	and (key=~a)")
(define (kv-get kv-handle key)
	(let ([row (pg-one-row (car kv-handle) kv-get-query
					(cdr kv-handle) key)])
		(pg-cell row 'value)))
(define kv-set-update-query "\
update _kv_ set
	value=~a
	where (namespace=~a)
	and (key=~a)")
(define kv-set-insert-query "\
insert into _kv_ (namespace, key, value)
	values (~a, ~a, ~a)")
(define (kv-set kv-handle key value)
	(let ([res
				(pg-exec (car kv-handle) kv-set-update-query
					value (cdr kv-handle) key)])
		(when (< (pg-cmd-tuples res) 1)
			(pg-exec (car kv-handle) kv-set-insert-query
				(cdr kv-handle) key value))))
(define kv-key-exists-query "\
select key
	from _kv_
	where (namespace=~a)
	and (key=~a)")
(define (kv-exists kv-handle key)
	(let ([row (pg-one-row (car kv-handle) kv-key-exists-query
					(cdr kv-handle) key)])
		(not (not row))))
(define kv-count-query "\
select count(*) as n
	from _kv_
	where (namespace=~a)")
(define (kv-count kv-handle)
	(let ([row (pg-one-row (car kv-handle) kv-count-query
					(cdr kv-handle))])
		(pg-cell row 'n)))
(define kv-keys-query "\
select key
	from _kv_
	where (namespace=~a)")
(define (kv-keys kv-handle)
	(let ([res (pg-exec (car kv-handle) kv-keys-query
					(cdr kv-handle))])
		(pg-map-rows res (lambda (row) (pg-cell row 'key)))))
(define kv-delete-query "\
delete from _kv_
	where (namespace=~a)
	and (key=~a)")
(define (kv-del kv-handle key)
	(pg-exec (car kv-handle) kv-delete-query
		(cdr kv-handle) key))
