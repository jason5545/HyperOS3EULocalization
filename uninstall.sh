#!/system/bin/sh

# 讓 Package Manager 在移除 systemless payload 後重新掃描。
rm -rf /data/system/package_cache/*
