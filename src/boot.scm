
(use-modules (ice-9 format))

(add-to-load-path gusher-root)

; responders
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

; postgres helpers
(define (pg-query dbh query args)
	; safely plug SQL query parameters into query template,
	; to be used with postgres calls below
	(apply format
		(append (list #f query)
			(map (lambda (item) (pg-format dbh item)) args))))
(define (pg-query-exec dbh query . args)
	(pg-exec dbh (pg-query dbh query args)))
(define (pg-query-one-row dbh query . args)
	(pg-one-row dbh (pg-query dbh query args)))
