(display "loading boot.scm\n")

(define html "
<html>
<head>
<title>Gusher!</title>
</head>
<body>
I am Gusher!
</body>
</html>
")

(define foo "
<html>
<head>
<title>Gusher!</title>
</head>
<body>
FOO!
</body>
</html>
")

(define http-handlers '())
(define (register-handler uri handler)
	(set! http-handlers (cons (cons uri handler) http-handlers))
	)
(define (plain-text body)
	(list 200 '(("content-type" . "text/plain")) body)
	)
(define (send-html body)
	(list 200 '(("content-type" . "text/html")) body)
	)
(register-handler "/"
	(lambda (headers query cookies)
		(send-html html)
		)
	)
(register-handler "/foo"
	(lambda (headers query cookies)
		(send-html foo)
		)
	)
(define (find-handler uri handlers)
	(cond 
		((null? handlers) #f)
		((equal? uri (caar handlers)) (cdar handlers))
		(#t (find-handler uri (cdr handlers)))
		)
	)

(define (dispatch conn headers query cookies)
	(let* ( (uri (assq-ref headers 'uri))
		(handler (find-handler uri http-handlers)) )
		(if handler
			(reply-http conn (handler headers query cookies))
			(reply-http conn (list 404
				'(("content-type" . "text/plain"))
				"Not Found"))
			)
		)
	)
