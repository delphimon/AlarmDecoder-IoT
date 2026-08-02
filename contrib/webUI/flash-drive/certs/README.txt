Optional HTTPS certificate directory

Copy the actual PEM file contents from Let's Encrypt or another certificate
authority here as:

  fullchain.pem
  privkey.pem

The private key must be unencrypted. FAT32 does not preserve Certbot symlinks,
so copy the files that those links point to. Enable HTTPS in ad2iot.ini and
restart the AD2IoT. Restart again whenever the certificate files are renewed.

Do not commit private keys to source control.
