
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

(export
	pg-format pg-exec pg-one-row pg-open-primitive pg-exec pg-close
		pg-cell pg-cmd-tuples pg-map-rows
	json-decode json-encode
	time-epoch time-now
	to-i
	http-port http-json http-get query-value)

(use-modules (gusher postgres))
