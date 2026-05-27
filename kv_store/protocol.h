/*
 * protocol.h - İstemci ve Sunucu Arasındaki Haberleşme Protokolü
 *
 * Bu başlık dosyası (header file), istemci (client.c) ile sunucu (server.c)
 * arasında Unix Domain Socket üzerinden gönderilecek mesajların yapısını tanımlar.
 * Hem istemci hem de sunucu bu dosyayı dahil ettiğinden, iki taraf da
 * aynı veri yapılarını kullanmış olur. Bu sayede iletişimde tutarlılık sağlanır.
 *
 * Neden .h dosyası kullandık?
 * C'de bir veri tipini veya sabiti birden fazla .c dosyasında kullanmak istediğimizde
 * kodu kopyalamak yerine bir header dosyasına yazarız ve sadece #include ile dahil ederiz.
 * Bu hem kod tekrarını önler hem de değişikliği tek yerden yapmamızı sağlar.
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

/* Sunucunun dinlediği Unix Domain Socket'in dosya yolu.
 * /tmp klasörü, geçici sistem dosyaları için kullanılan standart POSIX dizinidir. */
#define SOCKET_PATH "/tmp/kvstore.sock"

/* Anahtar (key) ve değer (value) için maksimum karakter uzunlukları */
#define MAX_KEY_LEN 128
#define MAX_VAL_LEN 256

/* Sunucunun istemciye döneceği mesaj için maksimum uzunluk.
 * LIST komutunda birden fazla satır döneceği için daha büyük tutuldu. */
#define MAX_MSG_LEN 8192

/*
 * CommandType - İstemciden Sunucuya Gönderilecek Komut Türleri
 * C'de enum (numaralandırma) kullanarak komut tiplerini sayısal değerlerle
 * eşleştiriyoruz. Bu, switch-case veya if-else kontrollerinde okunabilirliği artırır.
 */
typedef enum {
    CMD_SET,      /* Anahtar-değer çifti ekle veya güncelle */
    CMD_GET,      /* Anahtara karşılık gelen değeri getir */
    CMD_DELETE,   /* Anahtarı ve değerini sil */
    CMD_LIST,     /* Depodaki tüm anahtar-değer çiftlerini listele */
    CMD_UNKNOWN   /* Tanınmayan komut tipi (hata durumu için) */
} CommandType;

/*
 * Request - İstemciden Sunucuya Gönderilen İstek Yapısı
 * Socket üzerinden bu struct'ı binary olarak (write ile) gönderiyoruz.
 * Sunucu da (read ile) okuyunca aynı yapıya kavuşuyor.
 * Bu tekniğe "binary protocol" denir ve ağ programlamada yaygın kullanılır.
 */
typedef struct {
    CommandType type;       /* Hangi komut olduğu (SET, GET, DELETE, LIST) */
    char key[MAX_KEY_LEN];  /* İşlem yapılacak anahtar */
    char value[MAX_VAL_LEN];/* SET komutunda kullanılacak değer */
} Request;

/*
 * Response - Sunucudan İstemciye Dönen Cevap Yapısı
 * Sunucu isteği işledikten sonra bu struct'ı istemciye geri gönderir.
 * status: 0 ise başarı, -1 ise hata anlamına gelir. (UNIX geleneği)
 * message: GET ve LIST komutlarında sonuç; hata durumunda açıklama içerir.
 */
typedef struct {
    int status;               /* 0: başarı, -1: hata */
    char message[MAX_MSG_LEN];/* Dönen değer veya hata açıklaması */
} Response;

#endif /* PROTOCOL_H */
