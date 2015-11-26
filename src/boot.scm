
(use-modules (ice-9 format))

(add-to-load-path gusher-root)

; responders
(define (http-html path responder)
	; HTML response
	(http path
		(lambda (req)
			(simple-response "text/html; charset=UTF-8" (responder req)))))
(define (http-xml path responder)
	; XML responsee
	(http path
		(lambda (req)
			(simple-response "text/xml; charset=UTF-8" (responder req)))))
(define (http-text path responder)
	; plain text response
	(http path
		(lambda (req)
			(simple-response "text/plain; charset=UTF-8" (responder req)))))
(define (http-json path responder)
	; JSON response
	(http path
		(lambda (req)
			(let ([body (json-encode (responder req))])
				(json-response body)))))

; postgres helpers
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

; key-value store
(define (kv-open namespace read-only) (cons (pg-open) namespace))
(define (kv-close kv-handle) (pg-close (car kv-handle)))
(define kv-get-query "\
select value
	from _kv_
	where (namespace=~a)
	and (key=~a)")
(define (kv-get kv-handle key)
	(let ([row (pg-query-one-row (car kv-handle) kv-get-query
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
				(pg-query-exec (car kv-handle) kv-set-update-query
					value (cdr kv-handle) key)])
		(when (< (pg-cmd-tuples res) 1)
			(pg-query-exec (car kv-handle) kv-set-insert-query
				(cdr kv-handle) key value))))
(define kv-key-exists-query "\
select key
	from _kv_
	where (namespace=~a)
	and (key=~a)")
(define (kv-exists kv-handle key)
	(let ([row (pg-query-one-row (car kv-handle) kv-key-exists-query
					(cdr kv-handle) key)])
		(not (not row))))
(define kv-count-query "\
select count(*) as n
	from _kv_
	where (namespace=~a)")
(define (kv-count kv-handle)
	(let ([row (pg-query-one-row (car kv-handle) kv-count-query
					(cdr kv-handle))])
		(pg-cell row 'n)))
(define kv-keys-query "\
select key
	from _kv_
	where (namespace=~a)")
(define (kv-keys kv-handle)
	(let ([res (pg-query-exec (car kv-handle) kv-keys-query
					(cdr kv-handle))])
		(pg-map-rows res (lambda (row) (pg-cell row 'key)))))
(define kv-delete-query "\
delete from _kv_
	where (namespace=~a)
	and (key=~a)")
(define (kv-del kv-handle key)
	(pg-query-exec (car kv-handle) kv-delete-query
		(cdr kv-handle) key))
(define (session-key http-request)
	(assq-ref http-request 'session))
(define (session-read http-request)
	(let* ([kvh (kv-open "sessions" #f)]
			[session
				(kv-get kvh (session-key http-request))])
		(kv-close kvh)
		(and session (json-decode session))))
(define (session-get http-request key)
	(let ([session (session-read http-request)])
		(and session (assq-ref session key))))
(define (session-set http-request key value)
	(let* ([kvh (kv-open "sessions" #f)]
			[sess-key (session-key http-request)]
			[session-raw (kv-get kvh sess-key)]
			[session (or (json-decode (or session-raw "")) '())])
		(set! session (assq-set! session key value))
		(set! session
			(assq-set! session 'stamp (to-i (time-epoch (time-now)))))
		(kv-set kvh sess-key (json-encode session))
		(kv-close kvh)))
