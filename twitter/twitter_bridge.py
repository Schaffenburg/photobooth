import tweepy
import os
import socket
import urllib.request
 
# Consumer keys and access tokens, used for OAuth
consumer_key = ""
consumer_secret = ""
access_token = ""
access_token_secret = ""
 
auth = tweepy.OAuthHandler(consumer_key, consumer_secret)
auth.set_access_token(access_token, access_token_secret)
 
api = tweepy.API(auth)

# Creates the user object. The me() method returns the user whose authentication keys were used.
user = api.me()
 
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', 3000))
s.listen(1)

while True:
 try:
    os.remove("temp.jpg")
 except OSError:
    pass
 connection, address = s.accept()
 buf = connection.recv(64)
 if len(buf) > 0:
        buf = buf.strip()
        buf = buf.decode()
        urllib.request.urlretrieve(buf, "temp.jpg")
        status = "Insert text to post here" 
        api.update_with_media("temp.jpg", status)
        print(buf)
        del(buf)
