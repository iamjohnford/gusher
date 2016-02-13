(define-module (gusher misc)
	#:use-module (guile-user)
	#:use-module (ice-9 regex)
	#:export (deflate)
	)

(define deflate
	; wring out spans of whitespace;
	; for shrinking HTML or SQL passages indented
	; for readability
	(let ([pat (make-regexp "[ \t][ \t]+")])
		(lambda (src)
			(regexp-substitute/global #f pat
				(string-trim-both src) 'pre " " 'post))))
