from django.contrib import admin
from .models import Book, Location, BorrowRecord, RobotTaskLog

admin.site.register(Book)
admin.site.register(Location)
admin.site.register(BorrowRecord)
admin.site.register(RobotTaskLog)