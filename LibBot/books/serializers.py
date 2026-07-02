from rest_framework import serializers
from .models import Book, Location, BorrowRecord

class LocationSerializer(serializers.ModelSerializer):
    class Meta:
        model = Location
        fields = ['id', 'shelf', 'row', 'floor', 'map_image']

class BookSerializer(serializers.ModelSerializer):
    location = LocationSerializer(read_only=True)
    location_id = serializers.PrimaryKeyRelatedField(
        queryset=Location.objects.all(), source='location', write_only=True, required=False
    )

    class Meta:
        model = Book
        fields = ['id', 'title', 'author', 'isbn', 'category', 'quantity', 'is_available', 'location', 'location_id']

class BorrowRecordSerializer(serializers.ModelSerializer):
    class Meta:
        model = BorrowRecord
        fields = ['id', 'book', 'borrower_name', 'borrow_date', 'due_date', 'return_date']