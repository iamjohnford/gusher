(define-module (gusher messaging)
	#:use-module (guile-user)
	#:use-module (ice-9 format)
	#:use-module (ice-9 regex)
	#:use-module (gusher kv)
	#:use-module (gusher responders)
	#:export
		(msg-subscribe msg-unsubscribe msg-unsubscribe-all msg-publish))

(define msg-protocol "http")
(define (msg-dbconn) (kv-open "subscriptions"))
(define (msg-callback-url path)
	(format #f "~a://127.0.0.1:~d~a" msg-protocol http-port path))
(define (msg-get-callbacks db msg-key)
	; get all callbacks registered with a given message key
	(let ([json (kv-get db msg-key)])
		(or (and json (json-decode json)) '())))
(define (msg-callback-path msg-key)
	(format #f "/msg-~a" (symbol->string msg-key)))
(define (msg-subscribe msg-key handler)
	; Generate and register callback URL with the given message key,
	; then wrap handler in a JSON HTTP responder listening at that
	; URL
	(let* ([db (msg-dbconn)]
			[path (msg-callback-path msg-key)]
			[url (msg-callback-url path)]
			[callbacks (msg-get-callbacks db msg-key)])
		(unless (member url callbacks)
			(kv-set db msg-key
				(json-encode (cons url callbacks))))
		(kv-close db)
		(http-json path
			(lambda (req)
				(handler (json-decode (query-value req 'msg)))
				(list (cons 'status #t))))))
(define (msg-unsubscribe msg-key db)
	(let* ([url (msg-callback-url (msg-callback-path msg-key))]
			[keepers
				(filter (lambda (cb) (not (string=? cb url)))
					(msg-get-callbacks db msg-key))])
		(if (null? keepers)
			(kv-del db msg-key)
			(kv-set db msg-key (json-encode keepers)))))
(define (msg-unsubscribe-all)
	(let* ([db (msg-dbconn)])
		(for-each
			(lambda (key) (msg-unsubscribe (string->symbol key) db))
			(kv-keys db))
		(kv-close db)))
(define (msg-publish msg-key msg)
	; Get list of registered callback URLs for given message key
	; and send each a message. Message should be JSON-encodable.
	(let* ([db (msg-dbconn)])
		(for-each
			(lambda (callback)
				(http-get callback
					(cons 'post (list (cons "msg" msg)))))
			(msg-get-callbacks db msg-key))
		(kv-close db)))
