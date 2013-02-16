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

(define (plain-text conn body)
	(reply-http conn 200 '(("content-type" . "text/plain")) body)
	)
(define (send-html conn body)
	(reply-http conn 200 '(("content-type" . "text/html")) body)
	)

(define (default-handler conn headers query cookies)
	(let* (
		(uri (cdr (assq 'uri headers)))
		)
		(send-html conn html)
		)
	)
