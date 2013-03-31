
(define (http-html path responder)
	(http path
		(lambda (req) (simple-response "text/html" (responder req)))
		)
	)
(define (http-text path responder)
	(http path
		(lambda (req) (simple-response "text/plain" (responder req)))
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
						(cons "content-length" (string-length body)))
					body)
				)
			)
		)
	)

