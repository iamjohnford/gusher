gusher
======

Gusher is a multi-threaded
web application server written in C and embedding the
GNU [Guile](http://www.gnu.org/software/guile/) extension environment
to host applications written in
[Scheme](https://en.wikipedia.org/wiki/Scheme_%28programming_language%29).
A gusher web application can run stand-alone but will typically 
be fronted by a general-purpose web server such as apache or nginx.
Features include:

- basic [postgresql](http://www.postgresql.org/) interface for SQL
database access

- [KyotoCabinet](http://fallabs.com/kyotocabinet/), embedded
key-value database

- http client implemented with [libcurl](http://curl.haxx.se/)

- [Jansson](http://www.digip.org/jansson/)-based JSON encoder/decoder, to facilitate AJAX transactions and to receive JSON-encoded documents

- XML parsing with [libxml2](http://xmlsoft.org/)

- "make"-like content caching facility, where cache is automatically
refreshed as needed, according to a cached item's dependencies

- fast, simple templating facility, mainly for composing HTML

- use of Linux kernel's ["inotify"](http://linux.die.net/man/7/inotify)
subsystem to trigger application
events asynchronously in response to file system activity

- interactive command line editing and history (libreadline)
