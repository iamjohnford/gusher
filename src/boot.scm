
(add-to-load-path gusher-root)

(define (http-html path responder)
	(http path
		(lambda (req)
			(simple-response "text/html; charset=UTF-8" (responder req)))))
(define (http-xml path responder)
	(http path
		(lambda (req)
			(simple-response "text/xml; charset=UTF-8" (responder req)))))
(define (http-text path responder)
	(http path
		(lambda (req)
			(simple-response "text/plain; charset=UTF-8" (responder req)))))
(define (http-json path responder)
	(http path
		(lambda (req)
			(let ([body (json-encode (responder req))])
				(json-response body)))))

