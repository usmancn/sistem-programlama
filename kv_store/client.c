/*
 * client.c - Anahtar-Değer Deposu İstemcisi
 *
 * Bu program, komut satırından çalıştırılan bir istemci uygulamasıdır.
 * Sunucuya Unix Domain Socket üzerinden bağlanarak SET, GET, DELETE
 * ve LIST komutlarını gönderir ve sunucudan gelen cevabı ekrana yazdırır.
 *
 * Kullanım Örnekleri:
 *   ./client SET kullanici ahmet
 *   ./client GET kullanici
 *   ./client DELETE kullanici
 *   ./client LIST
 *
 * İletişim Protokolü:
 *   İstemci bir Request struct'ı gönderir, sunucu bir Response struct'ı döndürür.
 *   Struct'lar socket üzerinden binary olarak (write/read ile) iletilir.
 *   Bu basit ama etkili bir IPC (Inter-Process Communication) yöntemidir.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "protocol.h"

/*
 * print_usage() - Kullanım Bilgisi Göster
 *
 * Kullanıcı yanlış komut girdiğinde veya hiç argüman vermediğinde
 * ekrana yardım mesajı yazdırır.
 */
void print_usage() {
    printf("Kullanim:\n");
    printf("  ./client SET <anahtar> <deger>    - Anahtar-deger cifti ekle/guncelle\n");
    printf("  ./client GET <anahtar>             - Anahtarin degerini getir\n");
    printf("  ./client DELETE <anahtar>          - Anahtari sil\n");
    printf("  ./client LIST                      - Tum kayitlari listele\n");
}

int main(int argc, char* argv[]) {
    /* En az bir argüman (komut adı) verilmeli */
    if (argc < 2) {
        print_usage();
        return 1;
    }

    /* Sunucuya gönderilecek istek yapısını hazırla */
    Request req;
    memset(&req, 0, sizeof(Request)); /* Yapıyı sıfırla (güvenlik için) */

    /* Komut satırı argümanını parse et ve Request'i doldur */
    if (strcmp(argv[1], "SET") == 0) {
        /* SET için tam olarak 2 ek argüman gerekir: anahtar ve değer */
        if (argc != 4) {
            fprintf(stderr, "[HATA] SET komutu: ./client SET <anahtar> <deger>\n");
            return 1;
        }
        req.type = CMD_SET;
        strncpy(req.key,   argv[2], MAX_KEY_LEN - 1);
        strncpy(req.value, argv[3], MAX_VAL_LEN - 1);

    } else if (strcmp(argv[1], "GET") == 0) {
        /* GET için tam olarak 1 ek argüman gerekir: anahtar */
        if (argc != 3) {
            fprintf(stderr, "[HATA] GET komutu: ./client GET <anahtar>\n");
            return 1;
        }
        req.type = CMD_GET;
        strncpy(req.key, argv[2], MAX_KEY_LEN - 1);

    } else if (strcmp(argv[1], "DELETE") == 0) {
        /* DELETE için tam olarak 1 ek argüman gerekir: anahtar */
        if (argc != 3) {
            fprintf(stderr, "[HATA] DELETE komutu: ./client DELETE <anahtar>\n");
            return 1;
        }
        req.type = CMD_DELETE;
        strncpy(req.key, argv[2], MAX_KEY_LEN - 1);

    } else if (strcmp(argv[1], "LIST") == 0) {
        /* LIST için ek argüman gerekmez */
        req.type = CMD_LIST;

    } else {
        fprintf(stderr, "[HATA] Bilinmeyen komut: '%s'\n", argv[1]);
        print_usage();
        return 1;
    }

    /* ===== Unix Domain Socket ile Sunucuya Bağlan ===== */

    /* AF_UNIX: Yerel (local) socket ailesi - ağ değil dosya sistemi üzerinden iletişim
     * SOCK_STREAM: Güvenilir, bağlantı tabanlı (TCP benzeri) iletişim */
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("[HATA] Socket olusturulamadi");
        return 1;
    }

    /* Sunucu adresini yapılandır */
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    /* Sunucuya bağlan */
    if (connect(sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_un)) == -1) {
        perror("[HATA] Sunucuya baglanamadi (sunucu calisiyor mu?)");
        close(sock);
        return 1;
    }

    /* ===== İsteği Gönder ===== */
    /* Request struct'ını binary olarak socket'e yaz */
    if (write(sock, &req, sizeof(Request)) == -1) {
        perror("[HATA] Istek gonderilemedi");
        close(sock);
        return 1;
    }

    /* ===== Cevabı Al ===== */
    /* Sunucudan Response struct'ını binary olarak oku */
    Response res;
    ssize_t okunan = read(sock, &res, sizeof(Response));
    if (okunan <= 0) {
        perror("[HATA] Sunucudan cevap alinamadi");
        close(sock);
        return 1;
    }

    /* ===== Cevabı Ekrana Yazdır ===== */
    if (res.status == 0) {
        /* Başarılı işlem */
        if (req.type == CMD_GET || req.type == CMD_LIST) {
            /* GET ve LIST doğrudan değeri döndürür */
            printf("%s\n", res.message);
        } else {
            printf("Basarili: %s\n", res.message);
        }
    } else {
        /* Hata durumu */
        fprintf(stderr, "Hata: %s\n", res.message);
        close(sock);
        return 1;
    }

    close(sock); /* Bağlantıyı düzgünce kapat */
    return 0;
}
