(define-module (gusher session)
	#:use-module (guile-user)
	#:use-module (gusher kv)
	#:export (session-get session-set session-unset)
	)

(define (session-key http-request)
	(assq-ref http-request 'session))
(define (session-read http-request)
	(let* ([kvh (kv-open "sessions")]
			[session
				(kv-get kvh (session-key http-request))])
		(kv-close kvh)
		(and session (json-decode session))))
(define (session-get http-request key)
	(let ([session (session-read http-request)])
		(and session (assq-ref session key))))
(define (session-set http-request key value)
	(let* ([kvh (kv-open "sessions")]
			[sess-key (session-key http-request)]
			[session-raw (kv-get kvh sess-key)]
			[session (or (json-decode (or session-raw "")) '())])
		(set! session (assq-set! session key value))
		(set! session
			(assq-set! session 'stamp (to-i (time-epoch (time-now)))))
		(kv-set kvh sess-key (json-encode session))
		(kv-close kvh)))
(define (session-unset http-request key)
	(let* ([kvh (kv-open "sessions")]
			[sess-key (session-key http-request)]
			[session-raw (kv-get kvh sess-key)]
			[session (or (json-decode (or session-raw "")) '())])
		(set! session (assq-remove! session key))
		(set! session
			(assq-set! session 'stamp (to-i (time-epoch (time-now)))))
		(kv-set kvh sess-key (json-encode session))
		(kv-close kvh)))
