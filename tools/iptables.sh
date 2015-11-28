#!/bin/bash

#
# A la fin de la ligne POSTROUTING, assurez-vous de modifier l'adresse pour celle
# du serveur vpn.
#

iptables -F

# ---- Policies ----

iptables -P INPUT ACCEPT
iptables -P FORWARD DROP
iptables -P OUTPUT ACCEPT

# ---- Inputs ----

iptables -A INPUT -i lo -j ACCEPT
iptables -A INPUT ! -i lo -d 127.0.0.0/8 -j REJECT
iptables -A INPUT -m state --state ESTABLISHED,RELATED -j ACCEPT

# ssh
iptables -A INPUT -p tcp --dport 22 --j ACCEPT

# Log and drop everything else
iptables -A INPUT -j LOG --log-prefix "iptables_input "
iptables -A INPUT -j DROP

# ---- Outputs ----

iptables -A OUTPUT -o lo -d 127.0.0.0/8 -j ACCEPT
iptables -A OUTPUT -m state --state ESTABLISHED,RELATED -j ACCEPT

# ssh
iptables -A OUTPUT -p tcp -m multiport --dport 22 -j ACCEPT

# dns
iptables -A OUTPUT -p udp --dport 53 -j ACCEPT
iptables -A OUTPUT -p tcp --dport 53 -j ACCEPT

# ntp 
iptables -A OUTPUT -p udp --dport 123 -j ACCEPT

# pdoxd
iptables -A OUTPUT -p udp --dport 37777 -j ACCEPT

# DHCP
iptables -A OUTPUT -p udp --dport 67:68 --sport 67:68 -j ACCEPT

# Log and drop everything else
iptables -A OUTPUT -j LOG --log-prefix "iptables_output "
iptables -A OUTPUT -j DROP

# ---- Creation des fichiers de configuration ----

iptables-save >/root/firewall.rules
cp /root/firewall.rules /etc/iptables.up.rules

cat > /etc/network/if-pre-up.d/iptables << EOL
#!/bin/bash
/sbin/iptables-restore < /etc/iptables.up.rules
EOL

chmod 0755 /etc/network/if-pre-up.d/iptables
