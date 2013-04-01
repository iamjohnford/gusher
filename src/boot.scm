
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
				(json-response body)
				)
			)
		)
	)

