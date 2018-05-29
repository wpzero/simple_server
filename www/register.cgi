#!/usr/bin/ruby
require "cgi"
cgi = CGI.new
puts "Content-type:text/html\r\n"
puts "\r\n"
puts '<html>'
puts '<head>'
puts '<meta charset="UTF-8">'
puts '<title>POST</title>'
puts '</head>'
puts '<body>'
puts '<ul>'
cgi.params.each do |key, value|
  puts "<li>#{key}: #{value}</li>"
end
puts '</ul>'
puts '</body>'
puts '</html>'