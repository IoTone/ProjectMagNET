#!/usr/bin/env python3
"""catbot_traffic — conversation partners for a catbot v2 run.

Chats at the bot from every OTHER bridge pane on a jittered ~2-minute
cadence, phrases biased toward the bot's intents (food/nap/play/greet) so
the transcript is a dialogue, not a monologue. Stdlib only; talks to the
chat-bench bridge, never to serial.

    catbot_traffic.py [--url http://127.0.0.1:8642] [--bot neko]
"""
import argparse
import json
import random
import sys
import time
import urllib.request

PHRASES = [
    'hi neko how goes the mesh', 'anyone want food', 'time for a nap i think',
    'neko let us play', 'what do you see neko', 'good signal tonight',
    'who is hungry', 'chase the beacon neko', 'quiet night on channel 24',
    'tuna for everyone', 'sleepy mesh tonight', 'hello from the bench',
]


def post(url, path, obj):
    req = urllib.request.Request(url + path, json.dumps(obj).encode())
    return json.loads(urllib.request.urlopen(req, timeout=30).read())


def find_bot(url, name):
    """-> (bot_index, other_indexes) by asking each pane STATUS."""
    n = json.loads(urllib.request.urlopen(url + '/nodes', timeout=10).read())
    others, bot = [], None
    for i in range(n['count']):
        try:
            body = post(url, '/cmd', {'node': i, 'line': 'STATUS'})['body']
        except Exception:
            body = ''
        if f'name={name}' in body:
            bot = i
        else:
            others.append(i)
    if bot is None:
        sys.exit(f'no pane named {name} on the bridge')
    return bot, others


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:8642')
    ap.add_argument('--bot', default='neko')
    a = ap.parse_args()
    bot, senders = find_bot(a.url, a.bot)
    print(f'traffic: bot={a.bot}@{bot}, senders={senders}', flush=True)
    while True:
        time.sleep(random.randint(75, 140))
        try:
            post(a.url, '/send', {'node': random.choice(senders),
                                  'text': random.choice(PHRASES)})
        except Exception as e:
            print('traffic err:', e, flush=True)


if __name__ == '__main__':
    main()
