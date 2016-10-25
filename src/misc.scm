(define-module (gusher misc)
	#:use-module (guile-user)
	#:use-module (ice-9 regex)
	#:use-module (ice-9 format)
	#:export (deflate log-msg)
	)

(define deflate
	; wring out spans of whitespace;
	; for shrinking HTML or SQL passages indented
	; for readability
	(let ([pat (make-regexp "[ \t][ \t]+")])
		(lambda (src)
			(regexp-substitute/global #f pat
				(string-trim-both src) 'pre " " 'post))))

(define (log-msg fmt . args)
	(if (null? args)
		(log-msg-primitive fmt)
		(log-msg-primitive (apply format (cons #f (cons fmt args))))))
