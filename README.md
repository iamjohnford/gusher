gusher
======

Gusher is a [Scheme](https://en.wikipedia.org/wiki/Scheme_%28programming_language%29)-driven web application server built
on [Guile](http://www.gnu.org/software/guile/)
and [libmicrohttpd](http://www.gnu.org/software/libmicrohttpd/).
Gusher can run stand-alone as a web server, but is more intended to
be fronted by a general-purpose web server such as apache or nginx.
Other features include:

- bare-bones, wired-in [postgresql](http://www.postgresql.org/) interface

- [Redis](http://redis.io/) key-value storage

- [Jansson](http://www.digip.org/jansson/)-based JSON encoder, to facilitate AJAX transactions

- simple templating facility, mainly for composing HTML
