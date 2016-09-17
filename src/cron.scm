(define-module (gusher cron)
	#:use-module (guile-user)
	#:use-module (ice-9 format)
	#:use-module (gusher kv)
	#:export (cron-add cron-start))

(define crontab '())
(define (cron-add hour minute job)
	(set! crontab (cons (list hour minute job) crontab)))
(define cron-start
	(let ()
		(define (check-trigger trigger now time-part)
			; null trigger value implies "any"
			(or (null? trigger) (= (time-part now) trigger)))
		(define (runjob now hour minute job)
			; call job at appointed hour and minute
			(and
				(check-trigger hour now time-hour)
				(check-trigger minute now time-min)
				(job now)))
		(define (job-fail key . params)
			(log-msg (format #f "CRON FAIL: ~s, ~s" key params)))
		(define (runjobs now crontab)
			(for-each
				(lambda (cron-entry)
					(catch #t
						(lambda () (apply runjob (cons now cron-entry)))
						job-fail))
				crontab))
		(lambda ()
			(call-with-new-thread
				(lambda ()
					(let loop ()
						(when (snooze (- 60.1 (time-sec (time-now))))
							(runjobs (time-now) crontab)) (loop)))))))
