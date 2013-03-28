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

(define (simple-response mime-type content)
	(list "200 OK"
		(list (cons "content-type" mime-type)
			(cons "content-length"
				(number->string (string-length content))))
		content
		)
	)
(define (http-html path responder)
	(http path
		(lambda (req)
			(simple-response "text/html" (responder req))
			)
		)
	)
(define (http-text path responder)
	(http path
		(lambda (req)
			(simple-response "text/plain" (responder req))
			)
		)
	)
(define (http-json path responder)
	(http path
		(lambda (req)
			(let ((body (json-encode (responder req))))
				(list "200 OK"
					(list
						(cons "content-type" "text/json")
						(cons "cache-control" "max-age=0, must-revalidate")
						(cons "content-length" (number->string (string-length body)))
						)
					body
					)
				)
			)
		)
	)

(http-html "/index"   (lambda (req) html))
(http-html "/foo"     (lambda (req) foo))
(http-html "/foo/bar" (lambda (req) "BAR!"))
(http-text "/text"    (lambda (req) "just some text"))
(http-json "/json"    (lambda (req) '((foo . "bar") (cat . "horse"))))
