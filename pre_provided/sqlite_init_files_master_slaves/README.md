# SQLite Initialization Files

این بسته شامل فایل‌های لازم برای مقداردهی اولیه دیتابیس‌های SQLite در یک سیستم شامل یک Master و دو Slave است.

## فایل‌های قابل ارائه به دانشجو

```text
for_students/
├── scripts/
│   ├── master_init_db.sh
│   ├── slave1_init_db.sh
│   ├── slave2_init_db.sh
│   └── read_latest_sample.sh
└── data/
    ├── master_sensors.csv
    ├── slave1_sensors.csv
    └── slave2_sensors.csv
```

## فرمت CSV

```csv
sensor_id,sensor_type,sensor_name,location,value,unit,recorded_at
```

## نحوه اجرا

```bash
sudo apt update
sudo apt install -y sqlite3

cd for_students/scripts
chmod +x *.sh

./master_init_db.sh master.db ../data/master_sensors.csv
./slave1_init_db.sh slave1.db ../data/slave1_sensors.csv
./slave2_init_db.sh slave2.db ../data/slave2_sensors.csv
```

## تست نمونه

```bash
./read_latest_sample.sh master.db temperature 101
./read_latest_sample.sh slave1.db co2 204
./read_latest_sample.sh slave2.db smoke 304
```

## داده تست خصوصی

پوشه زیر برای استاد/طراح تمرین است و نباید به دانشجو داده شود:

```text
private_test_data_do_not_share/
```

این داده‌ها شناسه‌ها و مقادیر متفاوت دارند تا hard-code شدن پاسخ‌ها قابل تشخیص باشد.
