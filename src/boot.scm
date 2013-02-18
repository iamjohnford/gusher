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

(define (plain-text body)
	(list 200 '(("content-type" . "text/plain")) body)
	)
(define (send-html body)
	(list 200 '(("content-type" . "text/html")) body)
	)
(set-handler "/index"
	(lambda (headers query cookies)
		(send-html html)
		)
	)
(set-handler "/foo"
	(lambda (headers query cookies)
		(send-html foo)
		)
	)
(set-handler "/foo/bar"
	(lambda (headers query cookies)
		(send-html "BAR!")
		)
	)
