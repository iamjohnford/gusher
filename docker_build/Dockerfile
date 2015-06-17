FROM scalia/baseimage-wheezy
MAINTAINER Pete Yadlowsky, pmy@linux.com
RUN echo "build wheezy/gusher instance"
RUN apt-get -y update
RUN apt-get -y upgrade
RUN apt-get -y install apt-utils
RUN apt-get -y install guile-2.0 libjansson4 libpq5 libuuid1 libxml2 libdb5.1 libgcrypt11 libcurl3
RUN apt-get -y install git vim screen
RUN mkdir -p /var/lib/gusher/kv
RUN groupadd gusher
RUN chgrp -R gusher /var/lib/gusher
RUN chmod -R g+w /var/lib/gusher
RUN useradd -c Developer -d /home/dev -G gusher -m -s /bin/bash dev
WORKDIR /usr/local/bin
ADD gusher gusher
WORKDIR /var/lib/gusher
ADD boot.scm boot.scm
ADD http_msg.scm http_msg.scm
WORKDIR /
