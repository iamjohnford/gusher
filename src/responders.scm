; HTTP responders

(define-module (gusher responders)
	#:use-module (guile-user)
	#:export (http-html http-xml http-text http-json))

(define (http-html path responder)
	; HTML response
	(http path
		(lambda (req)
			(simple-response "text/html; charset=UTF-8" (responder req)))))
(define (http-xml path responder)
	; XML responsee
	(http path
		(lambda (req)
			(simple-response "text/xml; charset=UTF-8" (responder req)))))
(define (http-text path responder)
	; plain text response
	(http path
		(lambda (req)
			(simple-response "text/plain; charset=UTF-8" (responder req)))))
(define (http-json path responder)
	; JSON response
	(http path
		(lambda (req)
			(let ([body (json-encode (responder req))])
				(json-response body)))))
