gusher
======

Gusher is a multi-threaded
web application server written in C and embedding the
GNU [Guile](http://www.gnu.org/software/guile/) extension environment
to host applications written in
[Scheme](https://en.wikipedia.org/wiki/Scheme_%28programming_language%29).
Each gusher web application runs as a free-standing web server,
though will typically 
be fronted by a general-purpose web server such as apache or nginx.
Features include:

- basic [postgresql](http://www.postgresql.org/) interface for SQL
database access

- [KyotoCabinet](http://fallabs.com/kyotocabinet/), embedded
key-value database for storing session cookies and for general
high-speed caching

- http client implemented with [libcurl](http://curl.haxx.se/), for
fetching material (e.g. RSS content) from other web sources

- JSON encoder/decoder implemented with
[jansson](http://www.digip.org/jansson/), to facilitate AJAX
transactions and to receive JSON-encoded documents

- XML parsing with [libxml2](http://xmlsoft.org/), for receiving and
generating RSS feeds and the like

- [make](http://linux.die.net/man/1/make)-like content caching facility;
auto-refreshes cached items in response to changes in their dependencies

- fast, simple templating facility, for composing HTML, complex
SQL queries, anything that can be represented as a template

- asynchronous intra- and inter-application publish-subscribe messaging
via [0MQ](http://zeromq.org/)

- command line editing and history (libreadline) for interactive
Scheme coding
