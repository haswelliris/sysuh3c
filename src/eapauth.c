/*
 * =====================================================================================
 *
 *       Filename:  eapauth.c
 *
 *    Description:  eapauth client
 *
 *        Version:  1.0
 *        Created:  2013年05月24日 01时38分41秒
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Tyler Chung
 *   Organization:  SYSU
 *
 * =====================================================================================
 */

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <net/if.h>
#include "eapdef.h"
#include "eapauth.h"
#include "eaputils.h"
#include <stdio.h>
#include <stdarg.h>
#include <syslog.h>

int send_start(const eapauth_t *user);
int send_logoff(const eapauth_t *user);
int send_response_id(const eapauth_t *user, uint8_t packet_id);
int send_response_h3c(const eapauth_t *user, uint8_t packet_id);
int send_response_md5(const eapauth_t *user, uint8_t packet_id, const uint8_t *md5data, uint16_t datalen);

int eap_handler(const eapauth_t *user, const uint8_t *buf, size_t len);

void status_notify_func(int statno) {
    fprintf(stderr, "%s\n", strstat(statno));
}

void display_promote_func(int priority, const char *format, ...) {
    va_list arglist;
    va_start(arglist, format);
    fprintf(stderr, format, arglist);
    fprintf(stderr, "\n");
    va_end(arglist);
}

static void (*status_notify)(int) = status_notify_func;
static void (*display_promote)(int, const char *, ...) = display_promote_func;

int eapauth_init(eapauth_t *user, const char *iface) {
    uint8_t mac_addr_buf[6] = {0};
    struct timeval timeout;
    struct ifreq ifr; 
    size_t i;

    if ((user->client_fd = socket(AF_PACKET, SOCK_RAW, htons(ETHERTYPE_PAE))) < 0) {
        display_promote(LOG_ERR, "socket %s", strerror(errno));
        return EAPAUTH_ERR;
    }

    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(user->client_fd, SOL_SOCKET, SO_RCVTIMEO, (char *) &timeout, sizeof(timeout));

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, sizeof(ifr.ifr_name));

    if (ioctl(user->client_fd, SIOCGIFFLAGS, &ifr) < 0) {
        display_promote(LOG_ERR, "ioctl %s", strerror(errno));
        return EAPAUTH_ERR;
    }

    if ((ifr.ifr_flags & IFF_UP) == 0) {
        shutdown(user->client_fd, SHUT_RDWR);
        display_promote(LOG_ERR, "Interface %s is not avaliable",
                __func__, __LINE__, iface);
        return EAPAUTH_ERR;
    }

    if (ioctl(user->client_fd, SIOCGIFHWADDR, &ifr) < 0) {
        display_promote(LOG_ERR, "ioctl %s", strerror(errno));
        return EAPAUTH_ERR;
    }

    for (i = 0; i < 6; ++ i)
        mac_addr_buf[i] = ifr.ifr_hwaddr.sa_data[i];

    if (ioctl(user->client_fd, SIOCGIFINDEX, &ifr) < 0) {
        display_promote(LOG_ERR, "ioctl %s", strerror(errno));
        return EAPAUTH_ERR;
    }

    memset(&user->addr, 0, sizeof(user->addr));
    user->addr.sll_family = AF_PACKET;
    user->addr.sll_ifindex = ifr.ifr_ifindex;
    user->addr.sll_protocol = htons(ETHERTYPE_PAE);

    if (bind(user->client_fd, (struct sockaddr *) &user->addr, sizeof(user->addr)) == -1) {
        display_promote(LOG_ERR, "bind %s", strerror(errno));
        return EAPAUTH_ERR;
    }

    get_ethernet_header(mac_addr_buf, PAE_GROUP_ADDR, ETHERTYPE_PAE, user->ethernet_header);

    return EAPAUTH_OK;
}

int eapauth_auth(const eapauth_t *user) {
    char buf[1600] = {0};
    int ret;
    socklen_t sock_addr_len = sizeof(user->addr);
    send_start(user);
    status_notify(EAPAUTH_AUTH_START);
    while (1) {
        ret = recvfrom(user->client_fd, buf, sizeof(buf), 0,
                           (struct sockaddr *) &user->addr, &sock_addr_len);
        if (ret <= 0)
            return EAPAUTH_ERR;

        ret = eap_handler(user, buf + sizeof(user->ethernet_header), 
                    ret - sizeof(user->ethernet_header));
        if (ret != 0) return EAPAUTH_ERR;
        else if (ret == 1) break;
    }
    return EAPAUTH_OK;
}

void eapauth_set_status_listener(void (*func)(int)) {
    status_notify = func;
}

void eapauth_redirect_promote(void (*func)(int, const char*, ...)) {
    display_promote = func;
}

int send_start(const eapauth_t *user) {
    uint8_t buf[64] = {0};
    uint8_t *p_buf = buf;
    int len;

    if (user == NULL) return EAPAUTH_ERR;

    memcpy(p_buf, user->ethernet_header, sizeof(user->ethernet_header));
    p_buf += sizeof(user->ethernet_header);
    p_buf = get_EAPOL(EAPOL_START, NULL, 0, p_buf);
    len = sendto(user->client_fd, buf, p_buf - buf, MSG_NOSIGNAL,
            (struct sockaddr *) &user->addr, sizeof(user->addr));
    if (len <= 0)
        return EAPAUTH_ERR;
    return EAPAUTH_OK;
}

int send_logoff(const eapauth_t *user) {
    uint8_t buf[64] = {0};
    uint8_t *p_buf = buf;
    int len;

    if (user == NULL) return EAPAUTH_ERR;

    memcpy(p_buf, user->ethernet_header, sizeof(user->ethernet_header));
    p_buf += sizeof(user->ethernet_header);
    p_buf = get_EAPOL(EAPOL_LOGOFF, NULL, 0, p_buf);

    len = sendto(user->client_fd, buf, p_buf - buf, MSG_NOSIGNAL,
            (struct sockaddr *) &user->addr, sizeof(user->addr));
    if (len <= 0)
        return EAPAUTH_ERR;
    return EAPAUTH_OK;
}

int send_response_id(const eapauth_t *user, uint8_t packet_id) {
    uint8_t eapbuf[128] = {0};
    uint8_t eappacket[128] = {0};
    uint8_t *p_buf;
    int len;

    if (user == NULL) return EAPAUTH_ERR;

    memcpy(eappacket, VERSION_INFO, sizeof(VERSION_INFO));
    strcpy(eappacket + sizeof(VERSION_INFO), user->name);

    p_buf = get_EAP(EAP_RESPONSE, packet_id, EAP_TYPE_ID, eappacket, sizeof(VERSION_INFO) + strlen(user->name), eapbuf);
    p_buf = get_EAPOL(EAPOL_EAPPACKET, eapbuf, p_buf - eapbuf, eappacket + sizeof(user->ethernet_header));
    memcpy(eappacket, user->ethernet_header, sizeof(user->ethernet_header));
    len = sendto(user->client_fd, eappacket, p_buf - eappacket,
            MSG_NOSIGNAL, (struct sockaddr *) &user->addr, sizeof(user->addr));
    if (len < 0)
        return EAPAUTH_ERR;
    return EAPAUTH_OK;
}

int send_response_h3c(const eapauth_t *user, uint8_t packet_id) {

    uint8_t packetbuf[128] = {0};
    uint8_t eapbuf[128] = {0};
    uint8_t *p_buf;
    int len;

    if (user == NULL) return EAPAUTH_ERR;

    packetbuf[0] = strlen(user->password);
    strcpy(packetbuf + 1, user->password);
    strcpy(packetbuf + packetbuf[0] + 1, user->name);

    p_buf = get_EAP(EAP_RESPONSE, packet_id, EAP_TYPE_H3C, packetbuf, packetbuf[0] + strlen(user->name) + 1, eapbuf);
    p_buf = get_EAPOL(EAPOL_EAPPACKET, eapbuf, p_buf - eapbuf, packetbuf + sizeof(user->ethernet_header));
    memcpy(packetbuf, user->ethernet_header, sizeof(user->ethernet_header));

    len = sendto(user->client_fd, packetbuf, p_buf - packetbuf, MSG_NOSIGNAL,
            (struct sockaddr *) &user->addr, sizeof(user->addr));
    if (len <= 0)
        return EAPAUTH_ERR;
    return EAPAUTH_OK;
}
int send_response_md5(const eapauth_t *user, uint8_t packet_id, const uint8_t *md5data, uint16_t datalen) {
    uint8_t chap[16] = {0};
    size_t i;
    uint8_t eapbuf[128] = {0};
    uint8_t packetbuf[128] = {0};
    uint8_t *p_buf;
    int len;

    if (user == NULL || md5data == NULL) return EAPAUTH_ERR;

    strcpy(chap, user->password);
    for (i = 0; i < 16; ++ i)
        chap[i] ^= md5data[i];

    packetbuf[0] = sizeof(chap);
    memcpy(packetbuf + 1, chap, sizeof(chap));
    strcpy(packetbuf + sizeof(chap) + 1, user->name);

    p_buf = get_EAP(EAP_RESPONSE, packet_id, EAP_TYPE_MD5, 
            packetbuf, sizeof(chap) + strlen(user->name) + 1, eapbuf);
    p_buf = get_EAPOL(EAPOL_EAPPACKET, eapbuf, p_buf - eapbuf, packetbuf + sizeof(user->ethernet_header));
    memcpy(packetbuf, user->ethernet_header, sizeof(user->ethernet_header));

    len = sendto(user->client_fd, packetbuf, p_buf - packetbuf, MSG_NOSIGNAL,
            (struct sockaddr *) &user->addr, sizeof(user->addr));
    if (len <= 0)
        return EAPAUTH_ERR;
    return EAPAUTH_OK;
}

int eap_handler(const eapauth_t *user, const uint8_t *eap_packet, size_t len) {
    eapol_t eapol_packet;
    eapol_packet.vers = eap_packet[0];
    eapol_packet.type = eap_packet[1];
    eapol_packet.eapol_len = ntohs(*((uint16_t *) &eap_packet[2]));
    
    if (eapol_packet.type != EAPOL_EAPPACKET) {
        status_notify(EAPAUTH_UNKNOWN_PACKET_TYPE);
        display_promote(LOG_ERR, "got unknown packet type: %x",
                __func__, __LINE__, eapol_packet.type);
        return EAPAUTH_ERR;
    }

    eapol_packet.eap.code = eap_packet[4];
    eapol_packet.eap.id = eap_packet[5];
    
    eapol_packet.eap.eap_len = ntohs(*(uint16_t *) &eap_packet[6]);

    switch (eapol_packet.eap.code) {
        case EAP_SUCCESS:
            {
                struct timeval timeout;
                status_notify(EAPAUTH_EAP_SUCCESS);
                timeout.tv_sec = 30;
                timeout.tv_usec = 0;
                setsockopt(user->client_fd, SOL_SOCKET, SO_RCVTIMEO, (char *) &timeout, sizeof(timeout));
            }
            break;
        case EAP_FAILURE:
            {
                struct timeval timeout;
                status_notify(EAPAUTH_EAP_FAILURE);
                timeout.tv_sec = 5;
                timeout.tv_usec = 0;
                setsockopt(user->client_fd, SOL_SOCKET, SO_RCVTIMEO, (char *) &timeout, sizeof(timeout));
            }
            return EAPAUTH_FAIL;
            break;
        case EAP_RESPONSE:
            status_notify(EAPAUTH_EAP_RESPONSE);
            break;
        case EAP_REQUEST:
            eapol_packet.eap.reqtype = eap_packet[8];
            eapol_packet.eap.datalen = eap_packet[9];
            eapol_packet.eap.data = &eap_packet[10];
            switch (eapol_packet.eap.reqtype) {
                case EAP_TYPE_ID:
                    status_notify(EAPAUTH_AUTH_ID);
                    if (send_response_id(user, eapol_packet.eap.id) != 0) {
                        display_promote(LOG_ERR, "send_response_id error");
                        return EAPAUTH_ERR;
                    }
                    break;
                case EAP_TYPE_H3C:
                    status_notify(EAPAUTH_AUTH_H3C);
                    if (send_response_h3c(user, eapol_packet.eap.id) != 0) {
                        display_promote(LOG_ERR, "send_response_h3c error");
                        return EAPAUTH_ERR;
                    }
                    break;
                case EAP_TYPE_MD5:
                    status_notify(EAPAUTH_AUTH_MD5);
                    if (send_response_md5(user, eapol_packet.eap.id, 
                            eapol_packet.eap.data, eapol_packet.eap.datalen) != 0) {

                        display_promote(LOG_ERR, "send_response_md5 error");
                        return EAPAUTH_ERR;
                    }
                    break;
                default:
                    status_notify(EAPAUTH_UNKNOWN_REQUEST_TYPE);
            }
            break;
        case 10:
            break;
        default:
            status_notify(EAPAUTH_UNKNOWN_EAP_CODE);
    }
    return EAPAUTH_OK;
}

int eapauth_logoff(const eapauth_t *user) {
    if (user == NULL) return EAPAUTH_ERR;
    
    return send_logoff(user);
}
