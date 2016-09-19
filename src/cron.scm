(define-module (gusher cron)
	#:use-module (guile-user)
	#:use-module (ice-9 format)
	#:use-module (gusher kv)
	#:export (cron-add cron-start cron-stop cron-clear))

(define crontab '())
(define cron-state 'stopped)
(define (running?) (eq? cron-state 'running))
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
		(define (crond)
			(set! cron-state 'running)
			(log-msg "start cron")
			(let loop ()
				(when (running?)
					(when (snooze (- 60.1 (time-sec (time-now))))
						(when (running?) (runjobs (time-now) crontab)))
					(loop)))
			(set! cron-state 'stopped)
			(log-msg "cron stopped"))
		(lambda ()
			(if (eq? cron-state 'stopped)
				(begin (call-with-new-thread crond) #t)
				(begin (log-msg "cron active") #f)))))
(define (cron-stop)
	(if (running?)
		(begin
			(log-msg "stop cron...")
			(set! cron-state 'stopping) #t)
		(begin (log-msg "cron stopping or stopped") #f)))
(define (cron-clear) (set! crontab '()))