#! /usr/local/bin/gusher -d
!#

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

(define home-t (cache-get-file "/home/pmy/gusher/home.html"))

(http-html "/index"
	(lambda (req)
		(fill-template home-t #t
			(list
				(cons 'header (assq-ref req 'session))
				)
			)
		)
	)
(http-html "/foo"     (lambda (req) (assq-ref req 'query-string)))
(http-html "/foo/bar" (lambda (req) "BAR!"))
(http-text "/text"    (lambda (req) "just some text"))
(http-json "/json"    (lambda (req) (assq-ref req 'query)))
